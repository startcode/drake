#include "drake/multibody/rigid_body_plant/rigid_body_plant_bridge.h"

#include <memory>
#include <utility>

#include "drake/common/drake_assert.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/systems/framework/basic_vector.h"
#include "drake/systems/framework/framework_common.h"

namespace drake {
namespace systems {

using geometry::Cylinder;
using geometry::FrameId;
using geometry::FrameIdVector;
using geometry::FramePoseVector;
using geometry::GeometryFrame;
using geometry::GeometryInstance;
using geometry::GeometrySystem;
using geometry::Mesh;
using geometry::Shape;
using geometry::Sphere;

template <typename T>
RigidBodyPlantBridge<T>::RigidBodyPlantBridge(
    const RigidBodyTree<T>* tree, GeometrySystem<T>* geometry_system)
    : tree_(tree) {
  DRAKE_THROW_UNLESS(tree_ != nullptr);
  DRAKE_THROW_UNLESS(geometry_system != nullptr);
  source_id_ = geometry_system->RegisterSource(this->get_name());

  // Declare the tree's pose input port -- don't need the index, it is always 0.
  const int vector_size =
      tree->get_num_positions() + tree->get_num_velocities();
  plant_state_port_ =
      this->DeclareInputPort(kVectorValued, vector_size).get_index();

  geometry_id_port_ = this->DeclareAbstractOutputPort(
                              &RigidBodyPlantBridge::AllocateFrameIdOutput,
                              &RigidBodyPlantBridge::CalcFrameIdOutput)
                          .get_index();
  geometry_pose_port_ = this->DeclareAbstractOutputPort(
                                &RigidBodyPlantBridge::AllocateFramePoseOutput,
                                &RigidBodyPlantBridge::CalcFramePoseOutput)
                            .get_index();
  RegisterTree(geometry_system);
}

template <typename T>
const OutputPort<T>& RigidBodyPlantBridge<T>::geometry_id_output_port() const {
  return this->get_output_port(geometry_id_port_);
}

template <typename T>
const OutputPort<T>& RigidBodyPlantBridge<T>::geometry_pose_output_port()
    const {
  return this->get_output_port(geometry_pose_port_);
}

template <typename T>
const InputPortDescriptor<T>&
RigidBodyPlantBridge<T>::rigid_body_plant_state_input_port() const {
  return this->get_input_port(plant_state_port_);
}

template <typename T>
void RigidBodyPlantBridge<T>::RegisterTree(GeometrySystem<T>* geometry_system) {
  // TODO(SeanCurtis-TRI): This treats all bodies in the tree as dynamic. Some
  // may be fixed to the world. In that case, the bodies should *not* be
  // registered, and the geometries should be registered as anchored.
  // This *may* lead to a correctness issues. If two bodies are both rigidly
  // fixed, then their geometries should *implicitly* not be considered for
  // collision. However, if both are included as dynamic bodies, penetrations
  // will be reported.

  // Load *dynamic* geometry
  const int body_count = static_cast<int>(tree_->get_bodies().size());
  if (body_count > 1) {  // more than just the world.
    body_ids_.reserve(body_count - 1);
    for (int i = 1; i < body_count; ++i) {
      const RigidBody<T>& body = *tree_->get_bodies()[i];
      FrameId body_id = geometry_system->RegisterFrame(
          source_id_,
          GeometryFrame(body.get_name(), Isometry3<double>::Identity(),
                        body.get_model_instance_id()));
      body_ids_.push_back(body_id);
      // TODO(SeanCurtis-TRI): Handle collision and visual elements differently.
      // For now, we're simply consuming the visual elements.
      for (const auto& visual_element : body.get_visual_elements()) {
        std::unique_ptr<Shape> shape;
        Isometry3<double> X_FG = visual_element.getLocalTransform();
        const DrakeShapes::Geometry& geometry = visual_element.getGeometry();
        switch (visual_element.getShape()) {
          case DrakeShapes::SPHERE: {
            auto sphere = dynamic_cast<const DrakeShapes::Sphere&>(geometry);
            shape = std::make_unique<Sphere>(sphere.radius);
            break;
          }
          case DrakeShapes::CYLINDER: {
            auto cylinder =
                dynamic_cast<const DrakeShapes::Cylinder&>(geometry);
            shape =
                std::make_unique<Cylinder>(cylinder.radius, cylinder.length);
            break;
          }
          case DrakeShapes::MESH: {
            auto mesh = dynamic_cast<const DrakeShapes::Mesh&>(geometry);
            if (mesh.uri_.find("package://") == 0) {
              shape = std::make_unique<Mesh>(mesh.uri_);
            } else {
              shape = std::make_unique<Mesh>(mesh.resolved_filename_);
            }
            break;
          }
          default:
            drake::log()->warn("Only spheres, cylinders, and (limited) meshes"
                               "are supported by RigidBodyPlantBridge");
        }
        if (shape) {
          geometry_system->RegisterGeometry(
              source_id_, body_id,
              std::make_unique<GeometryInstance>(
                  X_FG, std::move(shape)));
          DRAKE_DEMAND(shape == nullptr);
        }
      }
    }
  }

  // TODO(SeanCurtis-TRI): Handle geometry attached to the world.
}

template <typename T>
FramePoseVector<T> RigidBodyPlantBridge<T>::AllocateFramePoseOutput(
    const MyContext&) const {
  DRAKE_DEMAND(source_id_.is_valid());
  // Poses of the registered bodies in the world -- defaults to identity.
  std::vector<Isometry3<T>> X_WF(body_ids_.size(), Isometry3<T>::Identity());
  return FramePoseVector<T>(source_id_, X_WF);
}

template <typename T>
void RigidBodyPlantBridge<T>::CalcFramePoseOutput(
    const MyContext& context, FramePoseVector<T>* poses) const {
  std::vector<Isometry3<T>>& pose_data = poses->mutable_vector();
  DRAKE_ASSERT(poses->vector().size() == body_ids_.size());

  const BasicVector<T>& input_vector = *this->EvalVectorInput(context, 0);
  // Obtains the generalized positions from vector_base.
  const VectorX<T> q = input_vector.CopyToVector().head(
      tree_->get_num_positions());

  // Computes the poses of each body.
  KinematicsCache<T> cache = tree_->doKinematics(q);

  // Saves the poses of each body in the frame pose vector
  // NOTE: Body 0 is the world; we skip it.
  // TODO(SeanCurtis-TRI): When I start skipping rigidly fixed bodies, modify
  // this loop to account for them.
  const int world_body = 0;
  for (size_t i = 1; i < tree_->get_bodies().size(); ++i) {
    pose_data[i - 1] = tree_->relativeTransform(cache, world_body, i);
  }
}

template <typename T>
FrameIdVector RigidBodyPlantBridge<T>::AllocateFrameIdOutput(
    const MyContext&) const {
  DRAKE_DEMAND(source_id_.is_valid());
  FrameIdVector ids(source_id_, body_ids_);
  return ids;
}

template <typename T>
void RigidBodyPlantBridge<T>::CalcFrameIdOutput(
    const MyContext&, FrameIdVector* frame_ids) const {
  DRAKE_DEMAND(source_id_.is_valid());
  *frame_ids = FrameIdVector(source_id_, body_ids_);
}

// Explicitly instantiates on the most common scalar types.
template class RigidBodyPlantBridge<double>;

}  // namespace systems
}  // namespace drake
