#include "glscript/value.hpp"

#include "glscript/symbols.hpp"

namespace glscript {

std::string Value::type_name() const {
    if (is_unit()) {
        return "unit";
    }
    if (is_bool()) {
        return "bool";
    }
    if (is_int()) {
        return "int";
    }
    if (is_float()) {
        return "float";
    }
    if (!is_heap()) {
        return "unknown";
    }

    switch (as_heap()->kind) {
    case GcObject::Kind::String:
        return "String";
    case GcObject::Kind::Enum:
        return as_enum(*this)->variant->owner->qualified_name;
    case GcObject::Kind::Object:
        return as_object(*this)->type->qualified_name;
    case GcObject::Kind::Array:
        return "array";
    case GcObject::Kind::Closure:
        return "function";
    case GcObject::Kind::Continuation:
        return "continuation";
    }
    return "unknown";
}

StringObj* as_string(const Value& value) {
    return static_cast<StringObj*>(value.as_heap());
}

EnumObj* as_enum(const Value& value) {
    return static_cast<EnumObj*>(value.as_heap());
}

ObjectInstance* as_object(const Value& value) {
    return static_cast<ObjectInstance*>(value.as_heap());
}

ArrayObj* as_array(const Value& value) {
    return static_cast<ArrayObj*>(value.as_heap());
}

ClosureObj* as_closure(const Value& value) {
    return static_cast<ClosureObj*>(value.as_heap());
}

ContinuationObj* as_continuation(const Value& value) {
    return static_cast<ContinuationObj*>(value.as_heap());
}

void Heap::collect(const std::vector<Value*>& roots) {
    // Mark.
    std::vector<GcObject*> worklist;
    const auto visit = [&](GcObject* object) {
        if (object != nullptr && !object->marked) {
            object->marked = true;
            worklist.push_back(object);
        }
    };

    for (auto* root : roots) {
        if (root->is_heap()) {
            visit(root->as_heap());
        }
    }

    while (!worklist.empty()) {
        auto* object = worklist.back();
        worklist.pop_back();
        object->trace(visit);
    }

    // Sweep.
    std::size_t live = 0;
    std::size_t write = 0;
    for (std::size_t read = 0; read < objects_.size(); ++read) {
        auto& object = objects_[read];
        if (object->marked) {
            object->marked = false;
            objects_[write++] = std::move(object);
            ++live;
        }
    }
    objects_.resize(write);

    // Grow the trigger threshold relative to the live set.
    threshold_ = live * 2 < 1024 ? 1024 : live * 2;
}

} // namespace glscript
