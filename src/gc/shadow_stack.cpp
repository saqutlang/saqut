#include "gc/shadow_stack.hpp"
#include "runtime/isolate.hpp"

// Depo artık Isolate üyesidir (ADR-045, Faz 1): her iş parçacığı kendi
// shadow stack'ini görür. Tek iş parçacıklı çalışışta davranış birebir aynıdır
// (tek örnek vardır).
ShadowStack& jitShadowStack() {
    return Isolate::current().shadow;
}
