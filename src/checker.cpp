#include "glscript/checker.hpp"

#include "glscript/ast.hpp"
#include "glscript/parser.hpp"
#include "glscript/symbols.hpp"
#include "glscript/type.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace glscript {
namespace {

ast::Visibility from_host_visibility(HostVisibility visibility) {
    switch (visibility) {
    case HostVisibility::Local:
        return ast::Visibility::Local;
    case HostVisibility::Export:
        return ast::Visibility::Export;
    case HostVisibility::Pub:
        return ast::Visibility::Pub;
    case HostVisibility::PubExport:
        return ast::Visibility::PubExport;
    }
    return ast::Visibility::Local;
}

bool is_importable(ast::Visibility visibility) {
    return visibility == ast::Visibility::Pub || visibility == ast::Visibility::PubExport;
}

bool is_importable(const TypeSymbol& type) {
    return type.global || is_importable(type.visibility);
}

bool is_importable(const FunctionSymbol& function) {
    return is_importable(function.visibility);
}

std::string join_path(const std::vector<std::string>& path) {
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            result += "::";
        }
        result += path[i];
    }
    return result;
}

std::string short_name(const std::string& qualified_name) {
    const auto position = qualified_name.rfind("::");
    if (position == std::string::npos) {
        return qualified_name;
    }
    return qualified_name.substr(position + 2);
}

bool has_attribute(const ast::FunctionDecl& declaration, const std::string& name) {
    return std::any_of(declaration.attributes.begin(), declaration.attributes.end(),
                       [&](const ast::Attribute& attribute) {
                           return attribute.name == name;
                       });
}

bool builtin_type_from_name(const std::string& name, TypePtr& output) {
    if (name == "int" || name == "i32" || name == "i64") {
        output = types::integer();
        return true;
    }
    if (name == "float" || name == "f32" || name == "f64") {
        output = types::floating();
        return true;
    }
    if (name == "bool") {
        output = types::boolean();
        return true;
    }
    if (name == "String" || name == "string") {
        output = types::string();
        return true;
    }
    if (name == "dynamic" || name == "any") {
        output = types::dynamic();
        return true;
    }
    if (name == "unit" || name == "void") {
        output = types::unit();
        return true;
    }
    if (name == "never") {
        output = types::never();
        return true;
    }
    if (name == "error") {
        output = types::error();
        return true;
    }
    return false;
}

TypePtr substitute(const TypePtr& type, const std::unordered_map<std::string, TypePtr>& bindings) {
    if (type == nullptr) {
        return nullptr;
    }

    switch (type->kind) {
    case Type::Kind::Builtin:
        return type;
    case Type::Kind::TypeParameter: {
        const auto it = bindings.find(type->name);
        return it != bindings.end() ? it->second : type;
    }
    case Type::Kind::Named: {
        if (type->arguments.empty()) {
            return type;
        }
        std::vector<TypePtr> arguments;
        arguments.reserve(type->arguments.size());
        for (const auto& argument : type->arguments) {
            arguments.push_back(substitute(argument, bindings));
        }
        return make_named_type(type->name, std::move(arguments));
    }
    case Type::Kind::Function: {
        std::vector<TypePtr> parameters;
        parameters.reserve(type->arguments.size());
        for (const auto& parameter : type->arguments) {
            parameters.push_back(substitute(parameter, bindings));
        }
        return make_function_type(std::move(parameters), substitute(type->result, bindings));
    }
    }
    return type;
}

TypePtr enum_instance_type(const TypeSymbol& type,
                           const std::vector<std::string>& type_parameters) {
    std::vector<TypePtr> arguments;
    arguments.reserve(type_parameters.size());
    for (const auto& parameter : type_parameters) {
        arguments.push_back(make_type_parameter(parameter));
    }
    return make_named_type(type.qualified_name, std::move(arguments));
}

class CheckerImpl {
public:
    CheckerImpl(const HostRegistry& host, DiagnosticSink& diagnostics, ModuleLoader loader)
        : host_(host), diagnostics_(diagnostics), loader_(std::move(loader)) {
        global_scope_ = make_scope(nullptr);
    }

    bool check(const std::string& module_name, const std::string& path, std::string text);

    // Moves everything the bytecode compiler needs into a retained Program.
    // Only valid after check() has run, and only once.
    Program take_program();

private:
    struct HandlerContext {
        TypePtr operation_result;
        TypePtr handler_result;
    };

    struct Callee {
        enum class Kind {
            Error,
            Dynamic,
            Function,
            Value,
        };

        Kind kind = Kind::Error;
        FunctionSymbol* function = nullptr;
        TypeSymbol* owner = nullptr;
        TypePtr receiver_type;
        TypePtr value_type;
    };

    struct MemberMethod {
        FunctionSymbol* function = nullptr;
        TypeSymbol* owner = nullptr;
        TypePtr receiver_type;
    };

    const HostRegistry& host_;
    DiagnosticSink& diagnostics_;
    ModuleLoader loader_;

    std::vector<std::unique_ptr<SourceFile>> sources_;
    std::unordered_map<std::string, std::unique_ptr<ModuleInfo>> modules_;
    std::unordered_map<std::string, TypeSymbol*> types_by_qname_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::vector<std::unique_ptr<FunctionSymbol>> builtin_functions_;

    Scope* global_scope_ = nullptr;
    Scope* current_scope_ = nullptr;
    ModuleInfo* current_module_ = nullptr;
    std::vector<std::string> current_type_parameters_;
    TypePtr current_result_;
    int loop_depth_ = 0;
    std::vector<HandlerContext> handlers_;
    bool host_setup_ = false;

    Scope* make_scope(Scope* parent);
    void error(SourceSpan span, std::string message);
    void bind(const ast::Expr& expression, Binding binding);

    TypeSymbol* lookup_type(const std::string& name) const;
    TypeSymbol* find_type_by_qualified(const std::string& name) const;
    Symbol* lookup_value(const std::string& name) const;
    void define_value(const std::string& name, Symbol symbol, SourceSpan span);

    void setup_host();
    void create_host_type_placeholders();
    void resolve_host_signatures();
    void add_ambient_symbols();
    TypePtr resolve_host_type_ref(const TypeRef& reference, ModuleInfo& module,
                                  const std::vector<std::string>& type_parameters);
    FunctionSymbol* create_function_symbol(ModuleInfo& module, TypeSymbol* owner,
                                           const std::string& name);

    ModuleInfo* load_source_module(const std::string& name, const std::string& path,
                                   std::string text);
    ModuleInfo* load_imported_module(const std::string& name, SourceSpan span);
    void register_source_declarations(ModuleInfo& module);
    void process_imports(ModuleInfo& module);
    void add_local_symbols(ModuleInfo& module);
    void resolve_module_signatures(ModuleInfo& module);
    void check_all_bodies();

    TypePtr resolve_type(const ast::TypeExpr& expression);
    TypePtr resolve_type_path(const std::vector<std::string>& path, SourceSpan span,
                              const std::vector<ast::TypeExprPtr>* arguments);
    void resolve_function_signature(FunctionSymbol& function, const ast::FunctionDecl& declaration,
                                    const std::vector<std::string>& outer_type_parameters);
    void resolve_class_signature(ModuleInfo& module, TypeSymbol& type,
                                 const ast::ClassDecl& declaration);
    void resolve_enum_signature(ModuleInfo& module, TypeSymbol& type,
                                const ast::EnumDecl& declaration);
    void resolve_effect_signature(ModuleInfo& module, TypeSymbol& type,
                                  const ast::EffectDecl& declaration);

    void check_module_bodies(ModuleInfo& module);
    void check_class_body(ModuleInfo& module, TypeSymbol& type, const ast::ClassDecl& declaration);
    void check_function_body(FunctionSymbol& function, const ast::FunctionDecl& declaration,
                             Scope* parent_scope, const TypePtr& receiver_type);
    void check_handler_declaration(ModuleInfo& module, const ast::HandlerDecl& declaration);
    void add_type_members_to_scope(TypeSymbol& type, const TypePtr& instance_type, Scope* scope,
                                   const std::unordered_map<std::string, TypePtr>& bindings);

    TypePtr check_statement(const ast::Stmt& statement);
    TypePtr check_block(const ast::BlockStmt& block);
    TypePtr check_expression(const ast::Expr& expression);
    TypePtr check_assignment_target(const ast::Expr& expression, bool& valid);
    TypePtr check_identifier(const ast::IdentifierExpr& expression);
    TypePtr check_member(const ast::MemberExpr& expression);
    TypePtr check_call(const ast::CallExpr& expression);
    TypePtr check_binary(const ast::BinaryExpr& expression);
    TypePtr check_unary(const ast::UnaryExpr& expression);
    TypePtr check_lambda(const ast::LambdaExpr& expression);
    TypePtr check_handle(const ast::HandleExpr& expression);
    TypePtr check_resume(const ast::ResumeExpr& expression);
    TypePtr check_handler_case(const ast::HandlerCase& handler_case, TypeSymbol* default_effect,
                               const TypePtr& handler_result);

    Callee resolve_callee(const ast::Expr& callee);
    TypePtr check_function_call(const ast::CallExpr& expression, FunctionSymbol& function,
                                TypeSymbol* owner, const TypePtr& receiver_type);
    TypePtr check_value_call(const ast::CallExpr& expression, const TypePtr& function_type);
    bool find_field(TypeSymbol& type, const TypePtr& instance_type, const std::string& name,
                    TypePtr& output, FieldSymbol*& field) const;
    bool find_method(TypeSymbol& type, const TypePtr& instance_type, const std::string& name,
                     MemberMethod& output) const;

    bool unify(const TypePtr& pattern, const TypePtr& actual,
               std::unordered_map<std::string, TypePtr>& bindings,
               const std::unordered_set<std::string>& inferable);
    bool is_assignable(const TypePtr& from, const TypePtr& to) const;
    bool is_named_subtype(const TypePtr& from, const TypePtr& to) const;
    bool is_condition_type(const TypePtr& type) const;

    TypePtr find_operator(const TypePtr& left, const TypePtr& right, const std::string& op,
                          OperatorInfo*& operator_info, TypeSymbol*& operator_owner) const;
    TypePtr find_unary_operator(const TypePtr& operand, const std::string& op,
                                OperatorInfo*& operator_info) const;
    std::unordered_map<std::string, TypePtr> class_bindings(const TypeSymbol& type,
                                                            const TypePtr& instance) const;
    TypePtr function_type(const FunctionSymbol& function) const;
};

Scope* CheckerImpl::make_scope(Scope* parent) {
    auto scope = std::make_unique<Scope>();
    scope->parent = parent;
    auto* raw = scope.get();
    scopes_.push_back(std::move(scope));
    return raw;
}

void CheckerImpl::error(SourceSpan span, std::string message) {
    diagnostics_.error(span, std::move(message));
}

void CheckerImpl::bind(const ast::Expr& expression, Binding binding) {
    expression.binding = std::move(binding);
}

Program CheckerImpl::take_program() {
    Program program;
    program.modules = std::move(modules_);
    program.sources = std::move(sources_);
    program.scopes = std::move(scopes_);
    program.builtin_functions = std::move(builtin_functions_);
    program.types_by_qname = std::move(types_by_qname_);
    return program;
}

TypeSymbol* CheckerImpl::lookup_type(const std::string& name) const {
    for (auto* scope = current_scope_; scope != nullptr; scope = scope->parent) {
        const auto it = scope->types.find(name);
        if (it != scope->types.end()) {
            return it->second;
        }
    }
    return nullptr;
}

TypeSymbol* CheckerImpl::find_type_by_qualified(const std::string& name) const {
    const auto it = types_by_qname_.find(name);
    return it != types_by_qname_.end() ? it->second : nullptr;
}

Symbol* CheckerImpl::lookup_value(const std::string& name) const {
    for (auto* scope = current_scope_; scope != nullptr; scope = scope->parent) {
        const auto it = scope->values.find(name);
        if (it != scope->values.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

void CheckerImpl::define_value(const std::string& name, Symbol symbol, SourceSpan span) {
    if (current_scope_->values.contains(name)) {
        error(span, "redefinition of '" + name + "'");
        return;
    }
    current_scope_->values.emplace(name, std::move(symbol));
}

// ---------------------------------------------------------------------------
// Host setup
// ---------------------------------------------------------------------------

void CheckerImpl::setup_host() {
    if (host_setup_) {
        return;
    }
    host_setup_ = true;

    for (const auto& schema : host_.modules()) {
        auto module = std::make_unique<ModuleInfo>();
        module->name = schema->name;
        module->is_host = true;
        module->scope = make_scope(global_scope_);
        modules_.emplace(module->name, std::move(module));
    }

    create_host_type_placeholders();
    resolve_host_signatures();
    add_ambient_symbols();
}

void CheckerImpl::create_host_type_placeholders() {
    for (const auto& schema : host_.modules()) {
        auto module_it = modules_.find(schema->name);
        if (module_it == modules_.end()) {
            continue;
        }
        auto& module = *module_it->second;

        for (const auto& type_schema : schema->types) {
            auto type = std::make_unique<TypeSymbol>();
            type->name = short_name(type_schema->name);
            type->qualified_name = type_schema->name.find("::") != std::string::npos
                                       ? type_schema->name
                                       : module.name + "::" + type_schema->name;
            type->module = module.name;
            type->type_parameters = type_schema->type_parameters;
            type->visibility = from_host_visibility(type_schema->visibility);
            type->global = type_schema->global;
            type->is_host = true;

            switch (type_schema->kind) {
            case TypeSchema::Kind::Class:
                type->kind = TypeSymbol::Kind::Class;
                break;
            case TypeSchema::Kind::Enum:
                type->kind = TypeSymbol::Kind::Enum;
                break;
            case TypeSchema::Kind::Effect:
                type->kind = TypeSymbol::Kind::Effect;
                break;
            }

            auto* raw = type.get();
            module.types[type->name] = raw;
            types_by_qname_[type->qualified_name] = raw;
            module.owned_types.push_back(std::move(type));

            if (raw->global) {
                const auto [it, inserted] = global_scope_->types.emplace(raw->name, raw);
                if (!inserted && it->second != raw) {
                    error(SourceSpan{}, "duplicate global host type '" + raw->name + "'");
                }
            }
        }
    }
}

void CheckerImpl::resolve_host_signatures() {
    for (const auto& schema : host_.modules()) {
        auto module_it = modules_.find(schema->name);
        if (module_it == modules_.end()) {
            continue;
        }
        auto& module = *module_it->second;
        current_module_ = &module;
        current_scope_ = module.scope;

        for (const auto& type_schema : schema->types) {
            auto* type = module.types[short_name(type_schema->name)];
            if (type == nullptr) {
                continue;
            }

            current_type_parameters_ = type->type_parameters;

            if (type_schema->base.has_value()) {
                auto base = resolve_host_type_ref(*type_schema->base, module,
                                                  type->type_parameters);
                if (base != nullptr && base->kind == Type::Kind::Named) {
                    type->base = find_type_by_qualified(base->name);
                    type->base_arguments = base->arguments;
                    if (type->base == nullptr) {
                        error(SourceSpan{}, "unknown host base type '" + base->name + "'");
                    } else if (type->base->kind != TypeSymbol::Kind::Class) {
                        error(SourceSpan{}, "host base type '" + base->name + "' is not a class");
                    }
                }
            }

            for (const auto& field_schema : type_schema->fields) {
                FieldSymbol field;
                field.name = field_schema.name;
                field.type = resolve_host_type_ref(field_schema.type, module,
                                                   type->type_parameters);
                field.visibility = from_host_visibility(field_schema.visibility);
                field.index = type->fields.size();
                field.owner = type;
                if (type->fields.contains(field.name)) {
                    error(SourceSpan{}, "duplicate field '" + field.name + "' in host type '" +
                                            type->qualified_name + "'");
                }
                type->fields[field.name] = std::move(field);
            }

            for (const auto& function_schema : type_schema->methods) {
                auto* function = create_function_symbol(module, type, function_schema->name);
                function->is_static = function_schema->is_static;
                function->aborting = function_schema->aborting;
                function->visibility = from_host_visibility(function_schema->visibility);

                std::vector<std::string> type_parameters = type->type_parameters;
                type_parameters.insert(type_parameters.end(),
                                       function_schema->type_parameters.begin(),
                                       function_schema->type_parameters.end());
                function->type_parameters = type_parameters;
                function->owner_type_parameter_count = type->type_parameters.size();

                for (const auto& parameter_schema : function_schema->parameters) {
                    function->parameter_names.push_back(parameter_schema.name);
                    function->parameter_types.push_back(
                        resolve_host_type_ref(parameter_schema.type, module, type_parameters));
                }
                function->result = resolve_host_type_ref(function_schema->return_type, module,
                                                         type_parameters);
                function->is_effect_operation =
                    type->kind == TypeSymbol::Kind::Effect && !function_schema->attributes.empty()
                        ? true
                        : type->kind == TypeSymbol::Kind::Effect;
                function->host_call = function_schema->call;

                if (type->methods.contains(function->name)) {
                    error(SourceSpan{}, "duplicate method '" + function->name + "' in host type '" +
                                            type->qualified_name + "'");
                }
                type->methods[function->name] = function;
                if (type->kind == TypeSymbol::Kind::Effect) {
                    type->effect_operations.push_back(function);
                }
            }

            for (const auto& operator_schema : type_schema->operators) {
                OperatorInfo info;
                info.op = operator_schema.op;
                for (const auto& operand : operator_schema.operands) {
                    info.operands.push_back(
                        resolve_host_type_ref(operand, module, type->type_parameters));
                }
                info.result =
                    resolve_host_type_ref(operator_schema.result, module, type->type_parameters);
                info.host_call = operator_schema.call;
                type->operators.push_back(std::move(info));
            }

            for (const auto& variant_schema : type_schema->variants) {
                auto variant = std::make_unique<EnumVariantSymbol>();
                variant->name = variant_schema.name;
                variant->owner = type;
                for (const auto& payload : variant_schema.payload) {
                    variant->payload.push_back(
                        resolve_host_type_ref(payload, module, type->type_parameters));
                }
                variant->value_type = enum_instance_type(*type, type->type_parameters);
                auto* raw = variant.get();
                type->variants[variant->name] = raw;
                module.owned_variants.push_back(std::move(variant));

                if (!raw->payload.empty()) {
                    auto* constructor = create_function_symbol(module, type, raw->name);
                    constructor->is_static = true;
                    constructor->parameter_types = raw->payload;
                    constructor->result = raw->value_type;
                    type->methods[raw->name] = constructor;
                }
            }
        }

        for (const auto& function_schema : schema->functions) {
            auto* function = create_function_symbol(module, nullptr, function_schema->name);
            function->is_static = true;
            function->visibility = from_host_visibility(function_schema->visibility);
            function->type_parameters = function_schema->type_parameters;

            for (const auto& parameter_schema : function_schema->parameters) {
                function->parameter_names.push_back(parameter_schema.name);
                function->parameter_types.push_back(resolve_host_type_ref(
                    parameter_schema.type, module, function_schema->type_parameters));
            }
            function->result = resolve_host_type_ref(function_schema->return_type, module,
                                                     function_schema->type_parameters);
            function->host_call = function_schema->call;
            module.functions[function->name] = function;
        }
    }
}

void CheckerImpl::add_ambient_symbols() {
    for (const auto& module_entry : modules_) {
        auto& module = *module_entry.second;
        if (!module.is_host) {
            continue;
        }
        for (const auto& type_entry : module.types) {
            auto* type = type_entry.second;
            if (type->kind != TypeSymbol::Kind::Effect || !type->global) {
                continue;
            }
            for (auto* operation : type->effect_operations) {
                if (global_scope_->values.contains(operation->name)) {
                    continue;
                }
                Symbol symbol;
                symbol.kind = Symbol::Kind::Function;
                symbol.function = operation;
                global_scope_->values[operation->name] = symbol;
            }
        }
    }

    auto abort = std::make_unique<FunctionSymbol>();
    abort->name = "abort";
    abort->qualified_name = "abort";
    abort->result = types::never();
    abort->is_static = true;
    auto* raw = abort.get();
    builtin_functions_.push_back(std::move(abort));

    Symbol symbol;
    symbol.kind = Symbol::Kind::Function;
    symbol.function = raw;
    global_scope_->values["abort"] = symbol;
}

TypePtr CheckerImpl::resolve_host_type_ref(const TypeRef& reference, ModuleInfo& module,
                                           const std::vector<std::string>& type_parameters) {
    switch (reference.kind) {
    case TypeRef::Kind::Builtin:
        return make_builtin_type(reference.builtin);
    case TypeRef::Kind::Error:
        return types::error();
    case TypeRef::Kind::TypeVariable: {
        if (std::find(type_parameters.begin(), type_parameters.end(), reference.name) ==
            type_parameters.end()) {
            error(SourceSpan{}, "unknown host type parameter '" + reference.name + "'");
            return types::error();
        }
        return make_type_parameter(reference.name);
    }
    case TypeRef::Kind::Named: {
        std::vector<TypePtr> arguments;
        for (const auto& argument : reference.arguments) {
            arguments.push_back(
                resolve_host_type_ref(argument, module, type_parameters));
        }

        TypeSymbol* symbol = nullptr;
        if (reference.name.find("::") != std::string::npos) {
            symbol = find_type_by_qualified(reference.name);
        } else {
            const auto local = module.types.find(reference.name);
            if (local != module.types.end()) {
                symbol = local->second;
            } else {
                const auto global = global_scope_->types.find(reference.name);
                if (global != global_scope_->types.end()) {
                    symbol = global->second;
                }
            }
        }

        if (symbol == nullptr) {
            error(SourceSpan{}, "unknown host type '" + reference.name + "' in module '" +
                                    module.name + "'");
            return types::error();
        }

        if (arguments.size() != symbol->type_parameters.size()) {
            error(SourceSpan{}, "host type '" + symbol->qualified_name + "' expects " +
                                    std::to_string(symbol->type_parameters.size()) +
                                    " type argument(s), got " +
                                    std::to_string(arguments.size()));
        }

        return make_named_type(symbol->qualified_name, std::move(arguments));
    }
    }

    return types::error();
}

FunctionSymbol* CheckerImpl::create_function_symbol(ModuleInfo& module, TypeSymbol* owner,
                                                    const std::string& name) {
    auto function = std::make_unique<FunctionSymbol>();
    function->name = name;
    function->owner = owner;
    function->is_host = module.is_host;
    function->qualified_name =
        owner != nullptr ? owner->qualified_name + "::" + name : module.name + "::" + name;

    auto* raw = function.get();
    module.owned_functions.push_back(std::move(function));

    if (owner == nullptr) {
        module.functions[name] = raw;
    }

    return raw;
}

// ---------------------------------------------------------------------------
// Source modules and imports
// ---------------------------------------------------------------------------

ModuleInfo* CheckerImpl::load_imported_module(const std::string& name, SourceSpan span) {
    const auto existing = modules_.find(name);
    if (existing != modules_.end()) {
        return existing->second.get();
    }

    if (loader_) {
        auto source = loader_(name);
        if (source.has_value()) {
            return load_source_module(name, source->path, source->text);
        }
    }

    error(span, "cannot find module '" + name + "'");
    return nullptr;
}

ModuleInfo* CheckerImpl::load_source_module(const std::string& name, const std::string& path,
                                            std::string text) {
    if (const auto existing = modules_.find(name); existing != modules_.end()) {
        if (!existing->second->is_host) {
            error(SourceSpan{}, "duplicate module '" + name + "'");
            return existing->second.get();
        }
        // A source module must not silently share a host module's name: the
        // script's declarations would vanish while checking "succeeds".
        error(SourceSpan{}, "module '" + name +
                                "' is provided by the host; rename the script file");
        return existing->second.get();
    }

    auto file = std::make_unique<SourceFile>(path, std::move(text));
    auto* file_pointer = file.get();
    sources_.push_back(std::move(file));

    Parser parser(*file_pointer, diagnostics_);
    auto unit = std::make_unique<ast::ModuleUnit>(parser.parse_module());
    unit->name = name;

    auto module = std::make_unique<ModuleInfo>();
    module->name = name;
    module->path = path;
    module->unit = std::move(unit);
    module->scope = make_scope(global_scope_);

    auto* raw_module = module.get();
    modules_.emplace(name, std::move(module));

    raw_module->loading = true;
    register_source_declarations(*raw_module);
    process_imports(*raw_module);
    add_local_symbols(*raw_module);

    current_module_ = raw_module;
    current_scope_ = raw_module->scope;
    current_type_parameters_.clear();
    resolve_module_signatures(*raw_module);
    raw_module->loading = false;

    return raw_module;
}

void CheckerImpl::register_source_declarations(ModuleInfo& module) {
    if (module.unit == nullptr) {
        return;
    }

    for (const auto& declaration : module.unit->declarations) {
        if (const auto* class_decl = dynamic_cast<const ast::ClassDecl*>(declaration.get())) {
            auto type = std::make_unique<TypeSymbol>();
            type->kind = TypeSymbol::Kind::Class;
            type->name = class_decl->name;
            type->qualified_name = module.name + "::" + class_decl->name;
            type->module = module.name;
            type->type_parameters = class_decl->type_parameters;
            type->visibility = class_decl->visibility;
            type->span = class_decl->span;
            type->class_declaration = const_cast<ast::ClassDecl*>(class_decl);

            auto* raw = type.get();
            if (module.types.contains(raw->name)) {
                error(raw->span, "redefinition of type '" + raw->name + "'");
                continue;
            }
            module.types[raw->name] = raw;
            types_by_qname_[raw->qualified_name] = raw;
            module.owned_types.push_back(std::move(type));
            continue;
        }

        if (const auto* enum_decl = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
            auto type = std::make_unique<TypeSymbol>();
            type->kind = TypeSymbol::Kind::Enum;
            type->name = enum_decl->name;
            type->qualified_name = module.name + "::" + enum_decl->name;
            type->module = module.name;
            type->type_parameters = enum_decl->type_parameters;
            type->visibility = enum_decl->visibility;
            type->span = enum_decl->span;
            type->enum_declaration = const_cast<ast::EnumDecl*>(enum_decl);

            auto* raw = type.get();
            if (module.types.contains(raw->name)) {
                error(raw->span, "redefinition of type '" + raw->name + "'");
                continue;
            }
            module.types[raw->name] = raw;
            types_by_qname_[raw->qualified_name] = raw;
            module.owned_types.push_back(std::move(type));
            continue;
        }

        if (const auto* effect_decl = dynamic_cast<const ast::EffectDecl*>(declaration.get())) {
            auto type = std::make_unique<TypeSymbol>();
            type->kind = TypeSymbol::Kind::Effect;
            type->name = effect_decl->name;
            type->qualified_name = module.name + "::" + effect_decl->name;
            type->module = module.name;
            type->visibility = effect_decl->visibility;
            type->span = effect_decl->span;
            type->effect_declaration = const_cast<ast::EffectDecl*>(effect_decl);

            auto* raw = type.get();
            if (module.types.contains(raw->name)) {
                error(raw->span, "redefinition of type '" + raw->name + "'");
                continue;
            }
            module.types[raw->name] = raw;
            types_by_qname_[raw->qualified_name] = raw;
            module.owned_types.push_back(std::move(type));
            continue;
        }

        if (const auto* function_decl =
                dynamic_cast<const ast::FunctionDecl*>(declaration.get())) {
            auto function = std::make_unique<FunctionSymbol>();
            function->name = function_decl->name;
            function->qualified_name = module.name + "::" + function_decl->name;
            function->visibility = function_decl->visibility;
            function->is_static = true;
            function->span = function_decl->span;
            function->declaration = const_cast<ast::FunctionDecl*>(function_decl);

            auto* raw = function.get();
            if (module.functions.contains(raw->name)) {
                error(raw->span, "redefinition of function '" + raw->name + "'");
                continue;
            }
            module.functions[raw->name] = raw;
            module.owned_functions.push_back(std::move(function));
        }
    }
}

void CheckerImpl::process_imports(ModuleInfo& module) {
    if (module.unit == nullptr) {
        return;
    }

    for (const auto& declaration : module.unit->declarations) {
        const auto* import = dynamic_cast<const ast::ImportDecl*>(declaration.get());
        if (import == nullptr) {
            continue;
        }

        const auto module_name = join_path(import->module_path);
        if (module_name.empty()) {
            error(import->span, "empty module path in import");
            continue;
        }

        auto* imported = load_imported_module(module_name, import->span);
        if (imported == nullptr) {
            continue;
        }

        auto import_type = [&](const std::string& name, SourceSpan span) {
            const auto it = imported->types.find(name);
            if (it == imported->types.end()) {
                error(span, "module '" + module_name + "' has no type named '" + name + "'");
                return;
            }
            if (!is_importable(*it->second)) {
                error(span, "type '" + name + "' from module '" + module_name +
                                "' is not importable (missing 'pub')");
                return;
            }
            if (module.scope->types.contains(name) && module.scope->types[name] != it->second) {
                error(span, "import of type '" + name + "' conflicts with an existing declaration");
                return;
            }
            module.scope->types[name] = it->second;
        };

        auto import_function = [&](const std::string& name, SourceSpan span) {
            const auto it = imported->functions.find(name);
            if (it == imported->functions.end()) {
                error(span, "module '" + module_name + "' has no function named '" + name + "'");
                return;
            }
            if (!is_importable(*it->second)) {
                error(span, "function '" + name + "' from module '" + module_name +
                                "' is not importable (missing 'pub')");
                return;
            }
            if (module.scope->values.contains(name) &&
                module.scope->values[name].function != it->second) {
                error(span, "import of function '" + name +
                                "' conflicts with an existing declaration");
                return;
            }
            Symbol symbol;
            symbol.kind = Symbol::Kind::Function;
            symbol.function = it->second;
            module.scope->values[name] = symbol;
        };

        if (import->wildcard) {
            for (const auto& [name, type] : imported->types) {
                if (is_importable(*type)) {
                    module.scope->types[name] = type;
                }
            }
            for (const auto& [name, function] : imported->functions) {
                if (is_importable(*function)) {
                    Symbol symbol;
                    symbol.kind = Symbol::Kind::Function;
                    symbol.function = function;
                    module.scope->values[name] = symbol;
                }
            }
            continue;
        }

        for (const auto& item : import->items) {
            if (imported->types.contains(item.name)) {
                import_type(item.name, item.span);
            } else if (imported->functions.contains(item.name)) {
                import_function(item.name, item.span);
            } else {
                error(item.span, "module '" + module_name + "' has no symbol named '" +
                                     item.name + "'");
            }
        }
    }
}

void CheckerImpl::add_local_symbols(ModuleInfo& module) {
    for (const auto& [name, type] : module.types) {
        module.scope->types[name] = type;
    }
    for (const auto& [name, function] : module.functions) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Function;
        symbol.function = function;
        module.scope->values[name] = symbol;
    }
}

// ---------------------------------------------------------------------------
// Signature resolution
// ---------------------------------------------------------------------------

TypePtr CheckerImpl::resolve_type_path(const std::vector<std::string>& path, SourceSpan span,
                                       const std::vector<ast::TypeExprPtr>* arguments) {
    std::vector<TypePtr> type_arguments;
    if (arguments != nullptr) {
        for (const auto& argument : *arguments) {
            type_arguments.push_back(resolve_type(*argument));
        }
    }

    if (path.empty()) {
        error(span, "expected type name");
        return types::error();
    }

    if (path.size() == 1) {
        const auto& name = path.front();

        if (std::find(current_type_parameters_.begin(), current_type_parameters_.end(), name) !=
            current_type_parameters_.end()) {
            if (!type_arguments.empty()) {
                error(span, "type parameter '" + name + "' cannot take type arguments");
            }
            return make_type_parameter(name);
        }

        if (TypePtr builtin; builtin_type_from_name(name, builtin)) {
            if (!type_arguments.empty()) {
                error(span, "builtin type '" + name + "' cannot take type arguments");
            }
            return builtin;
        }

        if (auto* symbol = lookup_type(name); symbol != nullptr) {
            if (type_arguments.size() != symbol->type_parameters.size()) {
                error(span, "type '" + symbol->qualified_name + "' expects " +
                                std::to_string(symbol->type_parameters.size()) +
                                " type argument(s), got " +
                                std::to_string(type_arguments.size()));
            }
            return make_named_type(symbol->qualified_name, std::move(type_arguments));
        }

        error(span, "unknown type '" + name + "'");
        return types::error();
    }

    const auto qualified = join_path(path);
    if (auto* symbol = find_type_by_qualified(qualified); symbol != nullptr) {
        if (type_arguments.size() != symbol->type_parameters.size()) {
            error(span, "type '" + symbol->qualified_name + "' expects " +
                            std::to_string(symbol->type_parameters.size()) +
                            " type argument(s), got " +
                            std::to_string(type_arguments.size()));
        }
        return make_named_type(symbol->qualified_name, std::move(type_arguments));
    }

    error(span, "unknown type '" + qualified + "'");
    return types::error();
}

TypePtr CheckerImpl::resolve_type(const ast::TypeExpr& expression) {
    if (const auto* dynamic_type = dynamic_cast<const ast::DynamicTypeExpr*>(&expression)) {
        (void)dynamic_type;
        return types::dynamic();
    }
    if (dynamic_cast<const ast::ErrorTypeExpr*>(&expression) != nullptr) {
        return types::error();
    }
    if (const auto* named = dynamic_cast<const ast::NamedTypeExpr*>(&expression)) {
        return resolve_type_path(named->path, named->span, &named->arguments);
    }
    error(expression.span, "unsupported type syntax");
    return types::error();
}

void CheckerImpl::resolve_function_signature(FunctionSymbol& function,
                                             const ast::FunctionDecl& declaration,
                                             const std::vector<std::string>& outer_type_parameters) {
    std::vector<std::string> type_parameters = outer_type_parameters;
    for (const auto& parameter : declaration.type_parameters) {
        if (std::find(type_parameters.begin(), type_parameters.end(), parameter) !=
            type_parameters.end()) {
            error(declaration.span, "duplicate type parameter '" + parameter + "'");
        }
        type_parameters.push_back(parameter);
    }
    function.type_parameters = type_parameters;
    function.owner_type_parameter_count = outer_type_parameters.size();
    function.aborting = has_attribute(declaration, "abort");

    current_type_parameters_ = type_parameters;

    std::unordered_set<std::string> parameter_names;
    for (const auto& parameter : declaration.parameters) {
        if (!parameter_names.insert(parameter.name).second) {
            error(parameter.span, "duplicate parameter '" + parameter.name + "'");
        }
        function.parameter_names.push_back(parameter.name);
        function.parameter_types.push_back(
            parameter.type != nullptr ? resolve_type(*parameter.type) : types::error());
    }

    function.result = declaration.return_type != nullptr ? resolve_type(*declaration.return_type)
                                                         : types::unit();
}

void CheckerImpl::resolve_class_signature(ModuleInfo& module, TypeSymbol& type,
                                          const ast::ClassDecl& declaration) {
    current_module_ = &module;
    current_scope_ = module.scope;
    current_type_parameters_ = type.type_parameters;

    if (declaration.base.has_value()) {
        const auto& base_name = *declaration.base;
        TypeSymbol* base = nullptr;
        if (base_name.find("::") != std::string::npos) {
            base = find_type_by_qualified(base_name);
        } else {
            base = lookup_type(base_name);
        }

        if (base == nullptr) {
            error(declaration.span, "unknown base class '" + base_name + "'");
        } else if (base->kind != TypeSymbol::Kind::Class) {
            error(declaration.span, "base type '" + base_name + "' is not a class");
        } else {
            type.base = base;
            std::vector<TypePtr> base_arguments;
            for (const auto& parameter : base->type_parameters) {
                base_arguments.push_back(make_type_parameter(parameter));
            }
            type.base_arguments = std::move(base_arguments);
        }
    }

    for (const auto& field : declaration.fields) {
        FieldSymbol symbol;
        symbol.name = field->name;
        symbol.type = field->type != nullptr ? resolve_type(*field->type) : types::error();
        symbol.visibility = field->visibility;
        symbol.span = field->span;
        symbol.index = type.fields.size();
        symbol.owner = &type;

        if (type.fields.contains(symbol.name)) {
            error(field->span, "duplicate field '" + symbol.name + "'");
            continue;
        }
        type.fields[symbol.name] = std::move(symbol);
    }

    for (const auto& method : declaration.methods) {
        auto* function = create_function_symbol(module, &type, method->name);
        function->is_static = false;
        function->visibility = method->visibility;
        function->span = method->span;
        function->declaration = method.get();
        resolve_function_signature(*function, *method, type.type_parameters);

        if (type.methods.contains(function->name)) {
            error(method->span, "duplicate method '" + function->name + "'");
            continue;
        }
        type.methods[function->name] = function;
    }
}

void CheckerImpl::resolve_enum_signature(ModuleInfo& module, TypeSymbol& type,
                                         const ast::EnumDecl& declaration) {
    current_module_ = &module;
    current_scope_ = module.scope;
    current_type_parameters_ = type.type_parameters;

    for (const auto& variant_decl : declaration.variants) {
        auto variant = std::make_unique<EnumVariantSymbol>();
        variant->name = variant_decl.name;
        variant->owner = &type;
        variant->span = variant_decl.span;

        for (const auto& payload : variant_decl.payload) {
            variant->payload.push_back(resolve_type(*payload));
        }
        variant->value_type = enum_instance_type(type, type.type_parameters);

        auto* raw = variant.get();
        if (type.variants.contains(raw->name)) {
            error(variant_decl.span, "duplicate enum variant '" + raw->name + "'");
            continue;
        }
        type.variants[raw->name] = raw;
        module.owned_variants.push_back(std::move(variant));

        if (!raw->payload.empty()) {
            auto* constructor = create_function_symbol(module, &type, raw->name);
            constructor->is_static = true;
            constructor->span = variant_decl.span;
            constructor->parameter_types = raw->payload;
            constructor->result = raw->value_type;
            type.methods[raw->name] = constructor;
        }
    }
}

void CheckerImpl::resolve_effect_signature(ModuleInfo& module, TypeSymbol& type,
                                           const ast::EffectDecl& declaration) {
    current_module_ = &module;
    current_scope_ = module.scope;
    current_type_parameters_ = type.type_parameters;

    for (const auto& operation : declaration.operations) {
        auto* function = create_function_symbol(module, &type, operation->name);
        function->is_static = true;
        function->is_effect_operation = true;
        function->visibility = operation->visibility;
        function->span = operation->span;
        function->declaration = operation.get();
        resolve_function_signature(*function, *operation, type.type_parameters);

        if (type.methods.contains(function->name)) {
            error(operation->span, "duplicate effect operation '" + function->name + "'");
            continue;
        }
        type.methods[function->name] = function;
        type.effect_operations.push_back(function);
    }
}

void CheckerImpl::resolve_module_signatures(ModuleInfo& module) {
    if (module.unit == nullptr || module.signatures_resolved) {
        return;
    }

    module.signatures_resolved = true;
    current_module_ = &module;
    current_scope_ = module.scope;

    for (const auto& declaration : module.unit->declarations) {
        if (const auto* class_decl = dynamic_cast<const ast::ClassDecl*>(declaration.get())) {
            const auto it = module.types.find(class_decl->name);
            if (it != module.types.end()) {
                resolve_class_signature(module, *it->second, *class_decl);
            }
            continue;
        }

        if (const auto* enum_decl = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
            const auto it = module.types.find(enum_decl->name);
            if (it != module.types.end()) {
                resolve_enum_signature(module, *it->second, *enum_decl);
            }
            continue;
        }

        if (const auto* effect_decl = dynamic_cast<const ast::EffectDecl*>(declaration.get())) {
            const auto it = module.types.find(effect_decl->name);
            if (it != module.types.end()) {
                resolve_effect_signature(module, *it->second, *effect_decl);
            }
            continue;
        }

        if (const auto* function_decl =
                dynamic_cast<const ast::FunctionDecl*>(declaration.get())) {
            const auto it = module.functions.find(function_decl->name);
            if (it != module.functions.end()) {
                auto& function = *it->second;
                if (function.declaration == function_decl) {
                    resolve_function_signature(function, *function_decl, {});
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Body checking
// ---------------------------------------------------------------------------

void CheckerImpl::check_all_bodies() {
    for (auto& [name, module] : modules_) {
        (void)name;
        if (module->is_host || module->bodies_checked) {
            continue;
        }
        if (!module->signatures_resolved) {
            resolve_module_signatures(*module);
        }
        check_module_bodies(*module);
    }
}

void CheckerImpl::check_module_bodies(ModuleInfo& module) {
    if (module.bodies_checked) {
        return;
    }
    module.bodies_checked = true;
    current_module_ = &module;
    current_scope_ = module.scope;
    current_type_parameters_.clear();
    current_result_ = nullptr;
    loop_depth_ = 0;
    handlers_.clear();

    if (module.unit == nullptr) {
        return;
    }

    for (const auto& declaration : module.unit->declarations) {
        if (const auto* class_decl = dynamic_cast<const ast::ClassDecl*>(declaration.get())) {
            const auto it = module.types.find(class_decl->name);
            if (it != module.types.end()) {
                check_class_body(module, *it->second, *class_decl);
            }
            continue;
        }

        if (const auto* function_decl =
                dynamic_cast<const ast::FunctionDecl*>(declaration.get())) {
            const auto it = module.functions.find(function_decl->name);
            if (it != module.functions.end() && it->second->declaration == function_decl) {
                check_function_body(*it->second, *function_decl, module.scope, nullptr);
            }
            continue;
        }

        if (const auto* handler_decl =
                dynamic_cast<const ast::HandlerDecl*>(declaration.get())) {
            check_handler_declaration(module, *handler_decl);
        }
    }
}

std::unordered_map<std::string, TypePtr> CheckerImpl::class_bindings(
    const TypeSymbol& type, const TypePtr& instance) const {
    std::unordered_map<std::string, TypePtr> bindings;
    if (instance == nullptr || instance->kind != Type::Kind::Named ||
        instance->name != type.qualified_name) {
        return bindings;
    }
    if (instance->arguments.size() != type.type_parameters.size()) {
        return bindings;
    }
    for (std::size_t i = 0; i < type.type_parameters.size(); ++i) {
        bindings[type.type_parameters[i]] = instance->arguments[i];
    }
    return bindings;
}

bool CheckerImpl::find_field(TypeSymbol& type, const TypePtr& instance_type,
                             const std::string& name, TypePtr& output,
                             FieldSymbol*& field) const {
    const auto bindings = class_bindings(type, instance_type);

    const auto field_it = type.fields.find(name);
    if (field_it != type.fields.end()) {
        output = substitute(field_it->second.type, bindings);
        field = &field_it->second;
        return true;
    }

    if (type.base != nullptr) {
        std::vector<TypePtr> base_arguments;
        for (const auto& argument : type.base_arguments) {
            base_arguments.push_back(substitute(argument, bindings));
        }
        auto base_type = make_named_type(type.base->qualified_name, std::move(base_arguments));
        return find_field(*type.base, base_type, name, output, field);
    }

    return false;
}

bool CheckerImpl::find_method(TypeSymbol& type, const TypePtr& instance_type,
                              const std::string& name, MemberMethod& output) const {
    const auto method_it = type.methods.find(name);
    if (method_it != type.methods.end()) {
        output.function = method_it->second;
        output.owner = &type;
        output.receiver_type = instance_type;
        return true;
    }

    if (type.base != nullptr) {
        const auto bindings = class_bindings(type, instance_type);
        std::vector<TypePtr> base_arguments;
        for (const auto& argument : type.base_arguments) {
            base_arguments.push_back(substitute(argument, bindings));
        }
        auto base_type = make_named_type(type.base->qualified_name, std::move(base_arguments));
        return find_method(*type.base, base_type, name, output);
    }

    return false;
}

void CheckerImpl::add_type_members_to_scope(
    TypeSymbol& type, const TypePtr& instance_type, Scope* scope,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (type.base != nullptr) {
        std::vector<TypePtr> base_arguments;
        for (const auto& argument : type.base_arguments) {
            base_arguments.push_back(substitute(argument, bindings));
        }
        auto base_type = make_named_type(type.base->qualified_name, std::move(base_arguments));
        auto base_bindings = class_bindings(*type.base, base_type);
        add_type_members_to_scope(*type.base, base_type, scope, base_bindings);
    }

    for (auto& [name, field] : type.fields) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.type = substitute(field.type, bindings);
        symbol.field = &field;
        scope->values[name] = symbol;
    }

    for (const auto& [name, method] : type.methods) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Function;
        symbol.function = method;
        symbol.receiver_type = instance_type;
        scope->values[name] = symbol;
    }
}

void CheckerImpl::check_class_body(ModuleInfo& module, TypeSymbol& type,
                                   const ast::ClassDecl& declaration) {
    (void)module;
    const auto saved_scope = current_scope_;
    const auto saved_type_parameters = current_type_parameters_;
    const auto saved_result = current_result_;
    const auto saved_loop_depth = loop_depth_;

    auto instance_arguments = std::vector<TypePtr>();
    for (const auto& parameter : type.type_parameters) {
        instance_arguments.push_back(make_type_parameter(parameter));
    }
    auto instance_type = make_named_type(type.qualified_name, std::move(instance_arguments));
    auto bindings = class_bindings(type, instance_type);

    auto* class_scope = make_scope(module.scope);
    current_scope_ = class_scope;
    current_type_parameters_ = type.type_parameters;
    current_result_ = nullptr;
    loop_depth_ = 0;

    add_type_members_to_scope(type, instance_type, class_scope, bindings);

    for (const auto& field : declaration.fields) {
        if (field->initializer == nullptr) {
            continue;
        }
        const auto value_type = check_expression(*field->initializer);
        const auto field_it = type.fields.find(field->name);
        if (field_it != type.fields.end() && !is_assignable(value_type, field_it->second.type)) {
            error(field->initializer->span, "cannot initialize field '" + field->name +
                                                 "' of type " +
                                                 field_it->second.type->to_string() + " with " +
                                                 value_type->to_string());
        }
    }

    for (const auto& method : declaration.methods) {
        const auto it = type.methods.find(method->name);
        if (it == type.methods.end() || it->second->declaration != method.get()) {
            continue;
        }
        check_function_body(*it->second, *method, class_scope, instance_type);
    }

    current_scope_ = saved_scope;
    current_type_parameters_ = saved_type_parameters;
    current_result_ = saved_result;
    loop_depth_ = saved_loop_depth;
}

void CheckerImpl::check_function_body(FunctionSymbol& function,
                                      const ast::FunctionDecl& declaration,
                                      Scope* parent_scope, const TypePtr& receiver_type) {
    (void)receiver_type;
    const auto saved_scope = current_scope_;
    const auto saved_type_parameters = current_type_parameters_;
    const auto saved_result = current_result_;
    const auto saved_loop_depth = loop_depth_;

    auto* function_scope = make_scope(parent_scope);
    current_scope_ = function_scope;
    current_type_parameters_ = function.type_parameters;
    current_result_ = function.result;
    loop_depth_ = 0;

    for (std::size_t i = 0; i < function.parameter_names.size(); ++i) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.type = function.parameter_types[i];
        define_value(function.parameter_names[i], std::move(symbol), declaration.span);
    }

    if (declaration.body != nullptr) {
        check_statement(*declaration.body);
    }

    current_scope_ = saved_scope;
    current_type_parameters_ = saved_type_parameters;
    current_result_ = saved_result;
    loop_depth_ = saved_loop_depth;
}

void CheckerImpl::check_handler_declaration(ModuleInfo& module,
                                            const ast::HandlerDecl& declaration) {
    (void)module;
    TypeSymbol* effect = nullptr;
    if (!declaration.effect.empty()) {
        if (declaration.effect.find("::") != std::string::npos) {
            effect = find_type_by_qualified(declaration.effect);
        } else {
            effect = lookup_type(declaration.effect);
        }
        if (effect == nullptr) {
            error(declaration.span, "unknown effect '" + declaration.effect + "'");
        } else if (effect->kind != TypeSymbol::Kind::Effect) {
            error(declaration.span, "'" + declaration.effect + "' is not an effect");
            effect = nullptr;
        } else {
            const_cast<std::string&>(declaration.effect) = effect->qualified_name;
        }
    }

    if (declaration.handled_expression != nullptr) {
        check_expression(*declaration.handled_expression);
    }

    for (const auto& handler_case : declaration.cases) {
        check_handler_case(handler_case, effect, types::unit());
    }
}

// ---------------------------------------------------------------------------
// Statement checking
// ---------------------------------------------------------------------------

TypePtr CheckerImpl::check_block(const ast::BlockStmt& block) {
    auto* saved_scope = current_scope_;
    auto* block_scope = make_scope(saved_scope);
    current_scope_ = block_scope;

    for (const auto& statement : block.statements) {
        check_statement(*statement);
    }

    current_scope_ = saved_scope;
    return types::unit();
}

TypePtr CheckerImpl::check_statement(const ast::Stmt& statement) {
    if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
        return check_block(*block);
    }

    if (dynamic_cast<const ast::EmptyStmt*>(&statement) != nullptr) {
        return types::unit();
    }

    if (const auto* expression = dynamic_cast<const ast::ExprStmt*>(&statement)) {
        check_expression(*expression->expression);
        return types::unit();
    }

    if (const auto* declaration = dynamic_cast<const ast::VarDeclStmt*>(&statement)) {
        const auto type =
            declaration->type != nullptr ? resolve_type(*declaration->type) : types::error();

        if (declaration->initializer != nullptr) {
            const auto value_type = check_expression(*declaration->initializer);
            if (!is_assignable(value_type, type)) {
                error(declaration->initializer->span,
                      "cannot initialize '" + declaration->name + "' of type " +
                          type->to_string() + " with " + value_type->to_string());
            }
        }

        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.type = type;
        define_value(declaration->name, std::move(symbol), declaration->span);
        return types::unit();
    }

    if (const auto* assignment = dynamic_cast<const ast::AssignStmt*>(&statement)) {
        bool valid = false;
        const auto target_type = check_assignment_target(*assignment->target, valid);
        const auto value_type = check_expression(*assignment->value);
        if (valid && !is_assignable(value_type, target_type)) {
            error(assignment->value->span,
                  "cannot assign value of type " + value_type->to_string() + " to " +
                      target_type->to_string());
        }
        return types::unit();
    }

    if (const auto* return_statement = dynamic_cast<const ast::ReturnStmt*>(&statement)) {
        if (current_result_ == nullptr) {
            error(return_statement->span, "'return' is only valid inside a function");
            return types::never();
        }

        TypePtr value_type = types::unit();
        if (return_statement->value != nullptr) {
            value_type = check_expression(*return_statement->value);
        }

        if (!is_assignable(value_type, current_result_)) {
            error(return_statement->span,
                  "cannot return value of type " + value_type->to_string() + " from function returning " +
                      current_result_->to_string());
        }
        return types::never();
    }

    if (const auto* if_statement = dynamic_cast<const ast::IfStmt*>(&statement)) {
        const auto condition_type = check_expression(*if_statement->condition);
        if (!is_condition_type(condition_type)) {
            error(if_statement->condition->span,
                  "if condition must be bool, got " + condition_type->to_string());
        }
        check_statement(*if_statement->then_branch);
        if (if_statement->else_branch != nullptr) {
            check_statement(*if_statement->else_branch);
        }
        return types::unit();
    }

    if (const auto* while_statement = dynamic_cast<const ast::WhileStmt*>(&statement)) {
        const auto condition_type = check_expression(*while_statement->condition);
        if (!is_condition_type(condition_type)) {
            error(while_statement->condition->span,
                  "while condition must be bool, got " + condition_type->to_string());
        }
        ++loop_depth_;
        check_statement(*while_statement->body);
        --loop_depth_;
        return types::unit();
    }

    if (dynamic_cast<const ast::BreakStmt*>(&statement) != nullptr) {
        if (loop_depth_ == 0) {
            error(statement.span, "'break' is only valid inside a loop");
        }
        return types::never();
    }

    if (dynamic_cast<const ast::ContinueStmt*>(&statement) != nullptr) {
        if (loop_depth_ == 0) {
            error(statement.span, "'continue' is only valid inside a loop");
        }
        return types::never();
    }

    error(statement.span, "unsupported statement");
    return types::error();
}

// ---------------------------------------------------------------------------
// Expression checking
// ---------------------------------------------------------------------------

TypePtr CheckerImpl::function_type(const FunctionSymbol& function) const {
    return make_function_type(function.parameter_types, function.result);
}

TypePtr CheckerImpl::check_identifier(const ast::IdentifierExpr& expression) {
    if (expression.path.empty()) {
        error(expression.span, "empty identifier");
        return types::error();
    }

    if (expression.path.size() == 1) {
        const auto& name = expression.path.front();
        if (auto* symbol = lookup_value(name); symbol != nullptr) {
            switch (symbol->kind) {
            case Symbol::Kind::Variable:
                bind(expression, [&] {
                    Binding binding;
                    binding.kind = BindingKind::Variable;
                    binding.type = symbol->type;
                    binding.field = symbol->field;
                    return binding;
                }());
                return symbol->type;
            case Symbol::Kind::Function:
                bind(expression, [&] {
                    Binding binding;
                    binding.kind = BindingKind::Function;
                    binding.function = symbol->function;
                    binding.owner = symbol->function->owner;
                    binding.receiver_type = symbol->receiver_type;
                    return binding;
                }());
                return function_type(*symbol->function);
            case Symbol::Kind::EnumVariant:
                bind(expression, [&] {
                    Binding binding;
                    binding.kind = BindingKind::EnumVariant;
                    binding.variant = symbol->variant;
                    binding.type = symbol->variant->value_type;
                    return binding;
                }());
                return symbol->variant->value_type;
            }
        }

        if (lookup_type(name) != nullptr) {
            error(expression.span, "type '" + name + "' cannot be used as a value");
        } else {
            error(expression.span, "unknown identifier '" + name + "'");
        }
        return types::error();
    }

    if (expression.path.size() != 2) {
        error(expression.span, "invalid qualified name '" + join_path(expression.path) + "'");
        return types::error();
    }

    auto* owner = lookup_type(expression.path[0]);
    if (owner == nullptr) {
        error(expression.span, "unknown type '" + expression.path[0] + "'");
        return types::error();
    }

    const auto& member = expression.path[1];

    if (owner->kind == TypeSymbol::Kind::Enum) {
        const auto variant_it = owner->variants.find(member);
        if (variant_it != owner->variants.end()) {
            auto* variant = variant_it->second;
            if (!variant->payload.empty()) {
                const auto method_it = owner->methods.find(member);
                if (method_it != owner->methods.end()) {
                    bind(expression, [&] {
                        Binding binding;
                        binding.kind = BindingKind::Function;
                        binding.function = method_it->second;
                        binding.owner = owner;
                        return binding;
                    }());
                    return function_type(*method_it->second);
                }
            }
            bind(expression, [&] {
                Binding binding;
                binding.kind = BindingKind::EnumVariant;
                binding.variant = variant;
                binding.owner = owner;
                binding.type = variant->value_type;
                return binding;
            }());
            return variant->value_type;
        }
    }

    const auto method_it = owner->methods.find(member);
    if (method_it != owner->methods.end()) {
        if (!method_it->second->is_static &&
            owner->kind != TypeSymbol::Kind::Effect) {
            error(expression.span, "method '" + member + "' requires an instance");
            return types::error();
        }
        bind(expression, [&] {
            Binding binding;
            binding.kind = BindingKind::Function;
            binding.function = method_it->second;
            binding.owner = owner;
            return binding;
        }());
        return function_type(*method_it->second);
    }

    error(expression.span, "type '" + owner->qualified_name + "' has no static member '" + member +
                               "'");
    return types::error();
}

TypePtr CheckerImpl::check_member(const ast::MemberExpr& expression) {
    const auto object_type = check_expression(*expression.object);
    if (object_type->is_error()) {
        return object_type;
    }
    if (object_type->is_dynamic()) {
        return types::dynamic();
    }
    if (object_type->kind != Type::Kind::Named) {
        error(expression.span, "type " + object_type->to_string() + " has no members");
        return types::error();
    }

    auto* type = find_type_by_qualified(object_type->name);
    if (type == nullptr) {
        error(expression.span, "unknown type '" + object_type->name + "'");
        return types::error();
    }

    FieldSymbol* field_symbol = nullptr;
    if (TypePtr field_type;
        find_field(*type, object_type, expression.name, field_type, field_symbol)) {
        bind(expression, [&] {
            Binding binding;
            binding.kind = BindingKind::Field;
            binding.type = field_type;
            binding.owner = type;
            binding.field = field_symbol;
            return binding;
        }());
        return field_type;
    }

    MemberMethod method;
    if (find_method(*type, object_type, expression.name, method)) {
        if (!method.function->is_static) {
            error(expression.span, "method '" + expression.name + "' requires a call");
            return types::error();
        }
        bind(expression, [&] {
            Binding binding;
            binding.kind = BindingKind::StaticMethod;
            binding.function = method.function;
            binding.owner = method.owner;
            binding.receiver_type = method.receiver_type;
            return binding;
        }());
        return function_type(*method.function);
    }

    error(expression.span, "type '" + type->qualified_name + "' has no member '" + expression.name +
                               "'");
    return types::error();
}

CheckerImpl::Callee CheckerImpl::resolve_callee(const ast::Expr& callee) {
    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&callee)) {
        if (identifier->path.empty()) {
            return {};
        }

        if (identifier->path.size() == 1) {
            auto* symbol = lookup_value(identifier->path.front());
            if (symbol == nullptr) {
                if (lookup_type(identifier->path.front()) != nullptr) {
                    error(callee.span, "type '" + identifier->path.front() +
                                           "' cannot be called");
                } else {
                    error(callee.span, "unknown identifier '" + identifier->path.front() + "'");
                }
                return {};
            }

            Callee result;
            if (symbol->kind == Symbol::Kind::Function) {
                result.kind = Callee::Kind::Function;
                result.function = symbol->function;
                result.owner = symbol->function->owner;
                result.receiver_type = symbol->receiver_type;
                bind(*identifier, [&] {
                    Binding binding;
                    binding.kind = BindingKind::Function;
                    binding.function = symbol->function;
                    binding.owner = symbol->function->owner;
                    binding.receiver_type = symbol->receiver_type;
                    return binding;
                }());
            } else if (symbol->kind == Symbol::Kind::Variable &&
                       symbol->type->kind == Type::Kind::Function) {
                result.kind = Callee::Kind::Value;
                result.value_type = symbol->type;
            } else {
                error(callee.span, "'" + identifier->path.front() + "' is not callable");
            }
            return result;
        }

        if (identifier->path.size() != 2) {
            error(callee.span, "invalid qualified name '" + join_path(identifier->path) + "'");
            return {};
        }

        auto* owner = lookup_type(identifier->path[0]);
        if (owner == nullptr) {
            error(callee.span, "unknown type '" + identifier->path[0] + "'");
            return {};
        }

        const auto& member = identifier->path[1];

        if (owner->kind == TypeSymbol::Kind::Enum) {
            const auto variant_it = owner->variants.find(member);
            if (variant_it != owner->variants.end() && variant_it->second->payload.empty()) {
                error(callee.span, "enum variant '" + member + "' is not callable");
                return {};
            }
        }

        const auto method_it = owner->methods.find(member);
        if (method_it == owner->methods.end()) {
            error(callee.span, "type '" + owner->qualified_name + "' has no member '" + member +
                                   "'");
            return {};
        }

        auto* function = method_it->second;
        if (!function->is_static && owner->kind != TypeSymbol::Kind::Effect) {
            error(callee.span, "method '" + member + "' requires an instance");
            return {};
        }

        Callee result;
        result.kind = Callee::Kind::Function;
        result.function = function;
        result.owner = owner;
        bind(*identifier, [&] {
            Binding binding;
            binding.kind = BindingKind::Function;
            binding.function = function;
            binding.owner = owner;
            return binding;
        }());
        return result;
    }

    if (const auto* member = dynamic_cast<const ast::MemberExpr*>(&callee)) {
        const auto object_type = check_expression(*member->object);
        if (object_type->is_error()) {
            return {};
        }
        if (object_type->is_dynamic()) {
            Callee result;
            result.kind = Callee::Kind::Dynamic;
            return result;
        }
        if (object_type->kind != Type::Kind::Named) {
            error(member->span, "type " + object_type->to_string() + " has no methods");
            return {};
        }

        auto* type = find_type_by_qualified(object_type->name);
        if (type == nullptr) {
            error(member->span, "unknown type '" + object_type->name + "'");
            return {};
        }

        MemberMethod found;
        if (find_method(*type, object_type, member->name, found)) {
            Callee result;
            result.kind = Callee::Kind::Function;
            result.function = found.function;
            result.owner = found.owner;
            result.receiver_type = found.receiver_type;
            bind(*member, [&] {
                Binding binding;
                binding.kind = BindingKind::Function;
                binding.function = found.function;
                binding.owner = found.owner;
                binding.receiver_type = found.receiver_type;
                return binding;
            }());
            return result;
        }

        FieldSymbol* field_symbol = nullptr;
        if (TypePtr field_type; find_field(*type, object_type, member->name, field_type,
                                           field_symbol)) {
            error(member->span, "'" + member->name + "' is a field, not a method");
            return {};
        }

        error(member->span, "type '" + type->qualified_name + "' has no method '" + member->name +
                                "'");
        return {};
    }

    const auto type = check_expression(callee);
    if (type->is_dynamic()) {
        Callee result;
        result.kind = Callee::Kind::Dynamic;
        return result;
    }
    if (type->kind != Type::Kind::Function) {
        error(callee.span, "expression of type " + type->to_string() + " is not callable");
        return {};
    }

    Callee result;
    result.kind = Callee::Kind::Value;
    result.value_type = type;
    return result;
}

TypePtr CheckerImpl::check_value_call(const ast::CallExpr& expression,
                                      const TypePtr& function_type_value) {
    if (function_type_value->arguments.size() != expression.arguments.size()) {
        error(expression.span, "call expects " +
                                   std::to_string(function_type_value->arguments.size()) +
                                   " argument(s), got " +
                                   std::to_string(expression.arguments.size()));
    }

    for (std::size_t i = 0; i < expression.arguments.size(); ++i) {
        const auto argument_type = check_expression(*expression.arguments[i]);
        if (i < function_type_value->arguments.size() &&
            !is_assignable(argument_type, function_type_value->arguments[i])) {
            error(expression.arguments[i]->span,
                  "argument " + std::to_string(i + 1) + " expects " +
                      function_type_value->arguments[i]->to_string() + ", got " +
                      argument_type->to_string());
        }
    }

    return function_type_value->result;
}

TypePtr CheckerImpl::check_function_call(const ast::CallExpr& expression,
                                         FunctionSymbol& function, TypeSymbol* owner,
                                         const TypePtr& receiver_type) {
    std::unordered_map<std::string, TypePtr> bindings;
    std::unordered_set<std::string> inferable;

    if (owner != nullptr && !owner->type_parameters.empty()) {
        if (receiver_type != nullptr && receiver_type->kind == Type::Kind::Named &&
            receiver_type->name == owner->qualified_name &&
            receiver_type->arguments.size() == owner->type_parameters.size()) {
            for (std::size_t i = 0; i < owner->type_parameters.size(); ++i) {
                bindings[owner->type_parameters[i]] = receiver_type->arguments[i];
            }
        } else {
            for (const auto& parameter : owner->type_parameters) {
                inferable.insert(parameter);
            }
        }
    }

    for (std::size_t i = function.owner_type_parameter_count;
         i < function.type_parameters.size(); ++i) {
        inferable.insert(function.type_parameters[i]);
    }

    if (!expression.type_arguments.empty()) {
        std::vector<TypePtr> explicit_types;
        for (const auto& argument : expression.type_arguments) {
            explicit_types.push_back(resolve_type(*argument));
        }

        const auto own_type_parameter_count =
            function.type_parameters.size() - function.owner_type_parameter_count;

        if (explicit_types.size() != own_type_parameter_count) {
            error(expression.span, "function '" + function.name + "' expects " +
                                       std::to_string(own_type_parameter_count) +
                                       " explicit type argument(s), got " +
                                       std::to_string(explicit_types.size()));
        }

        for (std::size_t i = 0; i < explicit_types.size() &&
                                function.owner_type_parameter_count + i <
                                    function.type_parameters.size();
             ++i) {
            const auto& parameter =
                function.type_parameters[function.owner_type_parameter_count + i];
            bindings[parameter] = explicit_types[i];
            inferable.erase(parameter);
        }
    }

    if (expression.arguments.size() != function.parameter_types.size()) {
        error(expression.span, "function '" + function.name + "' expects " +
                                   std::to_string(function.parameter_types.size()) +
                                   " argument(s), got " +
                                   std::to_string(expression.arguments.size()));
    }

    std::vector<TypePtr> argument_types;
    argument_types.reserve(expression.arguments.size());
    for (const auto& argument : expression.arguments) {
        argument_types.push_back(check_expression(*argument));
    }

    for (std::size_t i = 0;
         i < function.parameter_types.size() && i < argument_types.size(); ++i) {
        const auto pattern = substitute(function.parameter_types[i], bindings);
        if (!unify(pattern, argument_types[i], bindings, inferable)) {
            error(expression.arguments[i]->span,
                  "argument " + std::to_string(i + 1) + " of '" + function.name +
                      "' expects " + pattern->to_string() + ", got " +
                      argument_types[i]->to_string());
        }
    }

    for (const auto& parameter : inferable) {
        if (!bindings.contains(parameter)) {
            error(expression.span, "could not infer type parameter '" + parameter + "' for '" +
                                       function.name + "'");
            bindings[parameter] = types::dynamic();
        }
    }

    for (std::size_t i = 0;
         i < function.parameter_types.size() && i < argument_types.size(); ++i) {
        const auto expected = substitute(function.parameter_types[i], bindings);
        if (!is_assignable(argument_types[i], expected)) {
            error(expression.arguments[i]->span,
                  "argument " + std::to_string(i + 1) + " of '" + function.name +
                      "' expects " + expected->to_string() + ", got " +
                      argument_types[i]->to_string());
        }
    }

    return substitute(function.result, bindings);
}

TypePtr CheckerImpl::check_call(const ast::CallExpr& expression) {
    auto callee = resolve_callee(*expression.callee);

    switch (callee.kind) {
    case Callee::Kind::Error:
        for (const auto& argument : expression.arguments) {
            check_expression(*argument);
        }
        return types::error();
    case Callee::Kind::Dynamic:
        for (const auto& argument : expression.arguments) {
            check_expression(*argument);
        }
        return types::dynamic();
    case Callee::Kind::Value:
        return check_value_call(expression, callee.value_type);
    case Callee::Kind::Function:
        return check_function_call(expression, *callee.function, callee.owner,
                                   callee.receiver_type);
    }

    return types::error();
}

TypePtr CheckerImpl::check_binary(const ast::BinaryExpr& expression) {
    const auto left = check_expression(*expression.left);
    const auto right = check_expression(*expression.right);

    if (left->is_error() || right->is_error()) {
        return types::error();
    }
    if (left->is_dynamic() || right->is_dynamic()) {
        return types::dynamic();
    }

    const auto& op = expression.op;

    if (op == "&&" || op == "||") {
        if (!is_condition_type(left) || !is_condition_type(right)) {
            error(expression.span, "operator '" + op + "' expects bool operands, got " +
                                       left->to_string() + " and " + right->to_string());
            return types::error();
        }
        return types::boolean();
    }

    if (op == "==" || op == "!=") {
        if (!is_assignable(left, right) && !is_assignable(right, left)) {
            error(expression.span, "cannot compare unrelated types " + left->to_string() +
                                       " and " + right->to_string());
            return types::error();
        }
        return types::boolean();
    }

    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        const auto left_numeric = left->is_builtin(BuiltinType::Int) ||
                                  left->is_builtin(BuiltinType::Float);
        const auto right_numeric = right->is_builtin(BuiltinType::Int) ||
                                   right->is_builtin(BuiltinType::Float);
        if (!left_numeric || !right_numeric) {
            error(expression.span, "operator '" + op + "' expects numeric operands, got " +
                                       left->to_string() + " and " + right->to_string());
            return types::error();
        }
        return types::boolean();
    }

    OperatorInfo* operator_info = nullptr;
    TypeSymbol* operator_owner = nullptr;
    if (auto custom = find_operator(left, right, op, operator_info, operator_owner);
        custom != nullptr) {
        bind(expression, [&] {
            Binding binding;
            binding.kind = BindingKind::Operator;
            binding.type = custom;
            binding.op = operator_info;
            binding.owner = operator_owner;
            return binding;
        }());
        return custom;
    }

    if (op == "+" && left->is_builtin(BuiltinType::String) &&
        right->is_builtin(BuiltinType::String)) {
        return types::string();
    }

    if (op == "%") {
        if (!left->is_builtin(BuiltinType::Int) || !right->is_builtin(BuiltinType::Int)) {
            error(expression.span, "operator '%' expects integer operands, got " +
                                       left->to_string() + " and " + right->to_string());
            return types::error();
        }
        return types::integer();
    }

    if (op == "+" || op == "-" || op == "*" || op == "/") {
        const auto left_numeric = left->is_builtin(BuiltinType::Int) ||
                                  left->is_builtin(BuiltinType::Float);
        const auto right_numeric = right->is_builtin(BuiltinType::Int) ||
                                   right->is_builtin(BuiltinType::Float);
        if (!left_numeric || !right_numeric) {
            error(expression.span, "operator '" + op + "' is not defined for " +
                                       left->to_string() + " and " + right->to_string());
            return types::error();
        }
        if (left->is_builtin(BuiltinType::Float) || right->is_builtin(BuiltinType::Float)) {
            return types::floating();
        }
        return types::integer();
    }

    error(expression.span, "unknown binary operator '" + op + "'");
    return types::error();
}

TypePtr CheckerImpl::check_unary(const ast::UnaryExpr& expression) {
    const auto operand = check_expression(*expression.operand);
    if (operand->is_error()) {
        return operand;
    }
    if (operand->is_dynamic()) {
        return types::dynamic();
    }

    if (expression.op == "!") {
        if (!is_condition_type(operand)) {
            error(expression.span, "operator '!' expects bool, got " + operand->to_string());
            return types::error();
        }
        return types::boolean();
    }

    if (expression.op == "-" || expression.op == "+") {
        OperatorInfo* operator_info = nullptr;
        if (auto custom = find_unary_operator(operand, expression.op, operator_info);
            custom != nullptr) {
            bind(expression, [&] {
                Binding binding;
                binding.kind = BindingKind::Operator;
                binding.type = custom;
                binding.op = operator_info;
                return binding;
            }());
            return custom;
        }
        if (operand->is_builtin(BuiltinType::Int) || operand->is_builtin(BuiltinType::Float)) {
            return operand;
        }
        error(expression.span, "operator '" + expression.op + "' is not defined for " +
                                   operand->to_string());
        return types::error();
    }

    error(expression.span, "unknown unary operator '" + expression.op + "'");
    return types::error();
}

TypePtr CheckerImpl::check_lambda(const ast::LambdaExpr& expression) {
    auto* saved_scope = current_scope_;
    const auto saved_type_parameters = current_type_parameters_;
    const auto saved_result = current_result_;
    const auto saved_loop_depth = loop_depth_;

    auto* function_scope = make_scope(saved_scope);
    current_scope_ = function_scope;
    loop_depth_ = 0;

    std::vector<TypePtr> parameter_types;
    std::unordered_set<std::string> names;
    for (const auto& parameter : expression.parameters) {
        if (!names.insert(parameter.name).second) {
            error(parameter.span, "duplicate lambda parameter '" + parameter.name + "'");
        }
        auto type = parameter.type != nullptr ? resolve_type(*parameter.type) : types::error();
        parameter_types.push_back(type);
        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.type = type;
        define_value(parameter.name, std::move(symbol), parameter.span);
    }

    TypePtr result = types::unit();
    if (expression.return_type != nullptr) {
        result = resolve_type(*expression.return_type);
    }
    current_result_ = result;

    if (expression.body != nullptr) {
        check_statement(*expression.body);

        // Infer the result from a trailing expression when none is declared.
        if (expression.return_type == nullptr) {
            const auto* body_block =
                dynamic_cast<const ast::BlockStmt*>(expression.body.get());
            if (body_block != nullptr && !body_block->statements.empty()) {
                if (const auto* tail = dynamic_cast<const ast::ExprStmt*>(
                        body_block->statements.back().get());
                    tail != nullptr && tail->expression != nullptr) {
                    result = check_expression(*tail->expression);
                    if (result->is_builtin(BuiltinType::Never)) {
                        result = types::unit();
                    }
                }
            }
        }
    }

    current_scope_ = saved_scope;
    current_type_parameters_ = saved_type_parameters;
    current_result_ = saved_result;
    loop_depth_ = saved_loop_depth;

    return make_function_type(std::move(parameter_types), result);
}

TypePtr CheckerImpl::check_resume(const ast::ResumeExpr& expression) {
    if (handlers_.empty()) {
        error(expression.span, "'resume' is only valid inside an effect handler");
        return types::error();
    }

    const auto& context = handlers_.back();
    if (!expression.wildcard && expression.value != nullptr) {
        const auto value_type = check_expression(*expression.value);
        if (!is_assignable(value_type, context.operation_result)) {
            error(expression.value->span,
                  "resume value expects " + context.operation_result->to_string() + ", got " +
                      value_type->to_string());
        }
    }

    return context.handler_result != nullptr ? context.handler_result : types::dynamic();
}

TypePtr CheckerImpl::check_handler_case(const ast::HandlerCase& handler_case,
                                        TypeSymbol* default_effect,
                                        const TypePtr& handler_result) {
    TypeSymbol* effect = default_effect;
    if (!handler_case.effect.empty()) {
        if (handler_case.effect.find("::") != std::string::npos) {
            effect = find_type_by_qualified(handler_case.effect);
        } else {
            effect = lookup_type(handler_case.effect);
        }
        if (effect == nullptr) {
            error(handler_case.span, "unknown effect '" + handler_case.effect + "'");
            return types::error();
        }
    }

    if (effect == nullptr) {
        // A case written as a bare operation name matches that operation in
        // any global effect — mirroring the runtime's matcher, which treats
        // an unnamed case as matching regardless of effect.
        for (auto& module_entry : modules_) {
            auto& candidate_module = *module_entry.second;
            if (!candidate_module.is_host) {
                continue;
            }
            for (auto& type_entry : candidate_module.types) {
                TypeSymbol* candidate = type_entry.second;
                if (candidate->kind != TypeSymbol::Kind::Effect || !candidate->global) {
                    continue;
                }
                const auto operation = candidate->methods.find(handler_case.operation);
                if (operation == candidate->methods.end() ||
                    !operation->second->is_effect_operation) {
                    continue;
                }
                if (effect != nullptr && effect != candidate) {
                    error(handler_case.span,
                          "operation '" + handler_case.operation +
                              "' is ambiguous; name the effect in the handler case");
                    return types::error();
                }
                effect = candidate;
            }
        }
        if (effect == nullptr) {
            error(handler_case.span, "handler case has no effect");
            return types::error();
        }
    }
    if (effect->kind != TypeSymbol::Kind::Effect) {
        error(handler_case.span, "'" + effect->qualified_name + "' is not an effect");
        return types::error();
    }

    // Normalize the case's effect name to the qualified name so downstream
    // consumers (the bytecode compiler) don't need to re-resolve it.
    const_cast<std::string&>(handler_case.effect) = effect->qualified_name;

    const auto operation_it = effect->methods.find(handler_case.operation);
    if (operation_it == effect->methods.end() ||
        !operation_it->second->is_effect_operation) {
        error(handler_case.span, "effect '" + effect->qualified_name +
                                     "' has no operation '" + handler_case.operation + "'");
        return types::error();
    }

    auto& operation = *operation_it->second;

    if (handler_case.parameters.size() != operation.parameter_types.size()) {
        error(handler_case.span, "handler for '" + operation.name + "' expects " +
                                     std::to_string(operation.parameter_types.size()) +
                                     " parameter(s), got " +
                                     std::to_string(handler_case.parameters.size()));
    }

    const auto saved_scope = current_scope_;
    const auto saved_result = current_result_;
    const auto saved_loop_depth = loop_depth_;
    auto* handler_scope = make_scope(saved_scope);
    current_scope_ = handler_scope;
    // A `return` inside a handler case produces the operation's result (the
    // value delivered at the raise site when the case does not resume).
    current_result_ = operation.result;
    loop_depth_ = 0;

    for (std::size_t i = 0; i < handler_case.parameters.size(); ++i) {
        const auto& parameter = handler_case.parameters[i];
        TypePtr parameter_type =
            parameter.type != nullptr ? resolve_type(*parameter.type) : types::error();

        if (i < operation.parameter_types.size() &&
            !is_assignable(parameter_type, operation.parameter_types[i]) &&
            !is_assignable(operation.parameter_types[i], parameter_type)) {
            error(parameter.span,
                  "handler parameter '" + parameter.name + "' should be " +
                      operation.parameter_types[i]->to_string() + ", got " +
                      parameter_type->to_string());
        }

        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.type = i < operation.parameter_types.size() ? operation.parameter_types[i]
                                                           : parameter_type;
        define_value(parameter.name, std::move(symbol), parameter.span);
    }

    if (handler_case.result_type != nullptr) {
        const auto declared_result = resolve_type(*handler_case.result_type);
        if (!is_assignable(declared_result, operation.result) ||
            !is_assignable(operation.result, declared_result)) {
            error(handler_case.result_type->span,
                  "handler result type " + declared_result->to_string() +
                      " does not match operation result " + operation.result->to_string());
        }
    }

    handlers_.push_back(HandlerContext{operation.result, handler_result});
    if (handler_case.body != nullptr) {
        check_statement(*handler_case.body);
    }
    handlers_.pop_back();

    current_scope_ = saved_scope;
    current_result_ = saved_result;
    loop_depth_ = saved_loop_depth;
    return types::unit();
}

TypePtr CheckerImpl::check_handle(const ast::HandleExpr& expression) {
    const auto body_type = check_expression(*expression.body);
    for (const auto& handler_case : expression.cases) {
        check_handler_case(handler_case, nullptr, body_type);
    }
    return body_type;
}

TypePtr CheckerImpl::check_assignment_target(const ast::Expr& expression, bool& valid) {
    valid = false;

    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
        if (identifier->path.size() != 1) {
            error(expression.span, "invalid assignment target");
            return types::error();
        }

        auto* symbol = lookup_value(identifier->path.front());
        if (symbol == nullptr) {
            error(expression.span, "unknown identifier '" + identifier->path.front() + "'");
            return types::error();
        }
        if (symbol->kind != Symbol::Kind::Variable) {
            error(expression.span, "'" + identifier->path.front() + "' is not assignable");
            return types::error();
        }

        valid = true;
        bind(expression, [&] {
            Binding binding;
            binding.kind = BindingKind::Variable;
            binding.type = symbol->type;
            binding.field = symbol->field;
            return binding;
        }());
        return symbol->type;
    }

    if (const auto* member = dynamic_cast<const ast::MemberExpr*>(&expression)) {
        const auto object_type = check_expression(*member->object);
        if (object_type->is_error()) {
            return object_type;
        }
        if (object_type->is_dynamic()) {
            valid = true;
            return types::dynamic();
        }
        if (object_type->kind != Type::Kind::Named) {
            error(expression.span, "type " + object_type->to_string() + " has no fields");
            return types::error();
        }

        auto* type = find_type_by_qualified(object_type->name);
        if (type == nullptr) {
            error(expression.span, "unknown type '" + object_type->name + "'");
            return types::error();
        }

        FieldSymbol* field_symbol = nullptr;
        if (TypePtr field_type; find_field(*type, object_type, member->name, field_type,
                                           field_symbol)) {
            valid = true;
            bind(expression, [&] {
                Binding binding;
                binding.kind = BindingKind::Field;
                binding.type = field_type;
                binding.owner = type;
                binding.field = field_symbol;
                return binding;
            }());
            return field_type;
        }

        error(expression.span, "type '" + type->qualified_name + "' has no assignable field '" +
                                   member->name + "'");
        return types::error();
    }

    error(expression.span, "invalid assignment target");
    return types::error();
}

TypePtr CheckerImpl::check_expression(const ast::Expr& expression) {
    if (dynamic_cast<const ast::ErrorExpr*>(&expression) != nullptr) {
        return types::error();
    }

    if (dynamic_cast<const ast::IntLiteralExpr*>(&expression) != nullptr) {
        return types::integer();
    }
    if (dynamic_cast<const ast::FloatLiteralExpr*>(&expression) != nullptr) {
        return types::floating();
    }
    if (dynamic_cast<const ast::StringLiteralExpr*>(&expression) != nullptr) {
        return types::string();
    }
    if (dynamic_cast<const ast::BoolLiteralExpr*>(&expression) != nullptr) {
        return types::boolean();
    }
    if (dynamic_cast<const ast::UnitLiteralExpr*>(&expression) != nullptr) {
        return types::unit();
    }
    if (dynamic_cast<const ast::WildcardExpr*>(&expression) != nullptr) {
        error(expression.span, "'_' is only valid as a resume value");
        return types::error();
    }

    if (const auto* template_string = dynamic_cast<const ast::TemplateStringExpr*>(&expression)) {
        for (const auto& part : template_string->parts) {
            if (part.expression && part.expression_value != nullptr) {
                check_expression(*part.expression_value);
            }
        }
        return types::string();
    }

    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
        return check_identifier(*identifier);
    }

    if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
        return check_unary(*unary);
    }

    if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
        return check_binary(*binary);
    }

    if (const auto* downcast = dynamic_cast<const ast::DowncastExpr*>(&expression)) {
        check_expression(*downcast->operand);
        if (downcast->target != nullptr) {
            const auto target = resolve_type(*downcast->target);
            bind(*downcast, [&] {
                Binding binding;
                binding.kind = BindingKind::Downcast;
                binding.type = target;
                return binding;
            }());
            return target;
        }
        return types::error();
    }

    if (const auto* call = dynamic_cast<const ast::CallExpr*>(&expression)) {
        return check_call(*call);
    }

    if (const auto* member = dynamic_cast<const ast::MemberExpr*>(&expression)) {
        return check_member(*member);
    }

    if (const auto* index = dynamic_cast<const ast::IndexExpr*>(&expression)) {
        const auto object_type = check_expression(*index->object);
        check_expression(*index->index);
        if (object_type->is_dynamic()) {
            return types::dynamic();
        }
        error(expression.span, "indexing is not supported for type " +
                                   object_type->to_string());
        return types::error();
    }

    if (const auto* lambda = dynamic_cast<const ast::LambdaExpr*>(&expression)) {
        return check_lambda(*lambda);
    }

    if (const auto* resume = dynamic_cast<const ast::ResumeExpr*>(&expression)) {
        return check_resume(*resume);
    }

    if (const auto* handle = dynamic_cast<const ast::HandleExpr*>(&expression)) {
        return check_handle(*handle);
    }

    if (const auto* if_expression = dynamic_cast<const ast::IfExpr*>(&expression)) {
        const auto condition_type = check_expression(*if_expression->condition);
        if (!is_condition_type(condition_type)) {
            error(if_expression->condition->span,
                  "if condition must be bool, got " + condition_type->to_string());
        }
        const auto then_type = check_expression(*if_expression->then_expr);
        const auto else_type = check_expression(*if_expression->else_expr);
        if (is_assignable(then_type, else_type) && is_assignable(else_type, then_type)) {
            return then_type;
        }
        if (then_type->is_builtin(BuiltinType::Never)) {
            return else_type;
        }
        if (else_type->is_builtin(BuiltinType::Never)) {
            return then_type;
        }
        error(expression.span, "if branches have incompatible types " +
                                   then_type->to_string() + " and " +
                                   else_type->to_string());
        return types::error();
    }

    error(expression.span, "unsupported expression");
    return types::error();
}

// ---------------------------------------------------------------------------
// Type utilities
// ---------------------------------------------------------------------------

bool CheckerImpl::unify(const TypePtr& pattern, const TypePtr& actual,
                        std::unordered_map<std::string, TypePtr>& bindings,
                        const std::unordered_set<std::string>& inferable) {
    if (pattern == nullptr || actual == nullptr) {
        return false;
    }
    if (pattern->is_error() || actual->is_error()) {
        return true;
    }

    if (pattern->kind == Type::Kind::TypeParameter) {
        if (!inferable.contains(pattern->name)) {
            return is_assignable(actual, pattern);
        }

        const auto it = bindings.find(pattern->name);
        if (it == bindings.end()) {
            bindings[pattern->name] = actual;
            return true;
        }

        const auto& bound = it->second;
        if (types_equal(bound, actual)) {
            return true;
        }
        if (bound->is_dynamic()) {
            it->second = actual;
            return true;
        }
        if (actual->is_dynamic()) {
            return true;
        }
        if (bound->is_builtin(BuiltinType::Int) &&
            actual->is_builtin(BuiltinType::Float)) {
            it->second = actual;
            return true;
        }
        if (bound->is_builtin(BuiltinType::Float) &&
            actual->is_builtin(BuiltinType::Int)) {
            return true;
        }
        return false;
    }

    if (pattern->kind == Type::Kind::Named) {
        if (actual->is_dynamic()) {
            return true;
        }
        if (actual->kind != Type::Kind::Named || pattern->name != actual->name ||
            pattern->arguments.size() != actual->arguments.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pattern->arguments.size(); ++i) {
            if (!unify(pattern->arguments[i], actual->arguments[i], bindings, inferable)) {
                return false;
            }
        }
        return true;
    }

    if (pattern->kind == Type::Kind::Function) {
        if (actual->kind != Type::Kind::Function ||
            pattern->arguments.size() != actual->arguments.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pattern->arguments.size(); ++i) {
            if (!unify(pattern->arguments[i], actual->arguments[i], bindings, inferable)) {
                return false;
            }
        }
        return unify(pattern->result, actual->result, bindings, inferable);
    }

    return is_assignable(actual, pattern);
}

bool CheckerImpl::is_named_subtype(const TypePtr& from, const TypePtr& to) const {
    if (from == nullptr || to == nullptr || from->kind != Type::Kind::Named ||
        to->kind != Type::Kind::Named) {
        return false;
    }
    if (types_equal(from, to)) {
        return true;
    }

    auto* type = find_type_by_qualified(from->name);
    if (type == nullptr || type->base == nullptr) {
        return false;
    }

    auto bindings = class_bindings(*type, from);
    std::vector<TypePtr> base_arguments;
    for (const auto& argument : type->base_arguments) {
        base_arguments.push_back(substitute(argument, bindings));
    }
    auto base_type = make_named_type(type->base->qualified_name, std::move(base_arguments));
    return is_named_subtype(base_type, to);
}

bool CheckerImpl::is_assignable(const TypePtr& from, const TypePtr& to) const {
    if (from == nullptr || to == nullptr) {
        return false;
    }
    if (from->is_error() || to->is_error()) {
        return true;
    }
    if (from->is_dynamic() || to->is_dynamic()) {
        return true;
    }
    if (from->is_builtin(BuiltinType::Never)) {
        return true;
    }
    if (types_equal(from, to)) {
        return true;
    }
    if (from->is_builtin(BuiltinType::Int) && to->is_builtin(BuiltinType::Float)) {
        return true;
    }
    if (from->kind == Type::Kind::Named && to->kind == Type::Kind::Named) {
        return is_named_subtype(from, to);
    }
    return false;
}

bool CheckerImpl::is_condition_type(const TypePtr& type) const {
    return type != nullptr &&
           (type->is_builtin(BuiltinType::Bool) || type->is_dynamic() || type->is_error());
}

TypePtr CheckerImpl::find_operator(const TypePtr& left, const TypePtr& right,
                                   const std::string& op, OperatorInfo*& operator_info,
                                   TypeSymbol*& operator_owner) const {
    operator_info = nullptr;
    operator_owner = nullptr;
    auto search = [&](const TypePtr& candidate, const TypePtr& other) -> TypePtr {
        if (candidate == nullptr || candidate->kind != Type::Kind::Named) {
            return nullptr;
        }
        auto* type = find_type_by_qualified(candidate->name);
        if (type == nullptr) {
            return nullptr;
        }
        const auto bindings = class_bindings(*type, candidate);
        for (auto& overload : type->operators) {
            if (overload.op != op || overload.operands.size() != 2) {
                continue;
            }
            const auto operand = substitute(overload.operands[1], bindings);
            if (is_assignable(other, operand)) {
                operator_info = &overload;
                operator_owner = type;
                return substitute(overload.result, bindings);
            }
        }
        return nullptr;
    };

    if (auto result = search(left, right); result != nullptr) {
        return result;
    }
    return search(right, left);
}

TypePtr CheckerImpl::find_unary_operator(const TypePtr& operand, const std::string& op,
                                         OperatorInfo*& operator_info) const {
    operator_info = nullptr;
    if (operand == nullptr || operand->kind != Type::Kind::Named) {
        return nullptr;
    }
    auto* type = find_type_by_qualified(operand->name);
    if (type == nullptr) {
        return nullptr;
    }
    const auto bindings = class_bindings(*type, operand);
    for (auto& overload : type->operators) {
        if (overload.op == op && overload.operands.size() == 1) {
            operator_info = &overload;
            return substitute(overload.result, bindings);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool CheckerImpl::check(const std::string& module_name, const std::string& path,
                        std::string text) {
    setup_host();

    load_source_module(module_name, path, std::move(text));
    check_all_bodies();

    return !diagnostics_.has_errors();
}

} // namespace

class Frontend::Impl {
public:
    Impl(const HostRegistry& host, DiagnosticSink& diagnostics, ModuleLoader loader)
        : host(host), diagnostics(diagnostics), loader(std::move(loader)) {}

    const HostRegistry& host;
    DiagnosticSink& diagnostics;
    ModuleLoader loader;
};

Frontend::Frontend(const HostRegistry& host, DiagnosticSink& diagnostics, ModuleLoader loader)
    : impl_(std::make_unique<Impl>(host, diagnostics, std::move(loader))) {}

Frontend::~Frontend() = default;

Frontend::Frontend(Frontend&&) noexcept = default;
Frontend& Frontend::operator=(Frontend&&) noexcept = default;

bool Frontend::check_source(const std::string& module_name, const std::string& path,
                            std::string text) {
    const auto starting_errors = impl_->diagnostics.error_count();
    auto checker = std::make_unique<CheckerImpl>(impl_->host, impl_->diagnostics, impl_->loader);
    checker->check(module_name, path, std::move(text));
    program_ = checker->take_program();
    return impl_->diagnostics.error_count() == starting_errors;
}

bool Frontend::check_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        const auto starting_errors = impl_->diagnostics.error_count();
        impl_->diagnostics.error(SourceSpan{}, "cannot open script file '" + path + "'");
        return impl_->diagnostics.error_count() == starting_errors;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    const auto module_name = std::filesystem::path(path).stem().string();
    return check_source(module_name, path, buffer.str());
}

} // namespace glscript
