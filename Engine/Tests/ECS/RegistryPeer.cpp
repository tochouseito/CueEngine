#include "TestComponents.h"

namespace cue::tests
{
ecs::CompID component_id_from_peer()
{
    return ecs::ComponentID<Counter>();
}
} // namespace cue::tests
