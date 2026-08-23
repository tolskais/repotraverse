#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "history/build_info.hpp"
#include "history/ir.hpp"

namespace {
llvm::cl::OptionCategory category("element history extractor");
llvm::cl::opt<std::string> revision("source-revision",
                                    llvm::cl::init("working-tree"),
                                    llvm::cl::cat(category));
llvm::cl::opt<std::string> configuration("configuration",
                                         llvm::cl::init("default"),
                                         llvm::cl::cat(category));
llvm::cl::opt<std::string> build_variant("build-variant", llvm::cl::init("{}"),
                                         llvm::cl::cat(category));
llvm::cl::opt<std::string> context_fingerprint("context-fingerprint",
                                               llvm::cl::init("unspecified"),
                                               llvm::cl::cat(category));
llvm::cl::opt<std::string> source_blob("source-blob",
                                       llvm::cl::init("unspecified"),
                                       llvm::cl::cat(category));
llvm::cl::opt<std::string> project_root("project-root", llvm::cl::init(""),
                                        llvm::cl::cat(category));
llvm::cl::opt<std::string> repository_id("repository-id", llvm::cl::init(""),
                                         llvm::cl::cat(category));

std::string usr(const clang::Decl *declaration) {
  llvm::SmallString<128> value;
  if (clang::index::generateUSRForDecl(declaration, value))
    return {};
  std::string result(value);
  const auto *tag = llvm::dyn_cast<clang::TagDecl>(declaration);
  if (!tag || tag->getIdentifier())
    return result;
  const auto &sources = declaration->getASTContext().getSourceManager();
  const auto location = sources.getPresumedLoc(
      sources.getSpellingLoc(declaration->getCanonicalDecl()->getLocation()));
  if (!location.isValid())
    return result;
  std::filesystem::path file(location.getFilename());
  if (!project_root.empty()) {
    std::error_code error;
    const auto root =
        std::filesystem::weakly_canonical(project_root.getValue(), error);
    const auto canonical = std::filesystem::weakly_canonical(file, error);
    if (!error) {
      const auto relative = canonical.lexically_relative(root);
      if (!relative.empty() && *relative.begin() != "..")
        file = relative;
    }
  }
  return result + "@anonymous:" + file.generic_string() + ":" +
         std::to_string(location.getLine()) + ":" +
         std::to_string(location.getColumn());
}

std::string normalized_project_path(const std::string &path) {
  if (project_root.empty() || path.empty())
    return std::filesystem::path(path).generic_string();
  std::error_code error;
  const auto file = std::filesystem::weakly_canonical(path, error);
  if (error)
    return std::filesystem::path(path).generic_string();
  const auto root =
      std::filesystem::weakly_canonical(project_root.getValue(), error);
  if (error)
    return std::filesystem::path(path).generic_string();
  const auto relative = file.lexically_relative(root);
  if (!relative.empty() && *relative.begin() != "..")
    return relative.generic_string();
  return file.generic_string();
}

std::string logical_id(const clang::NamedDecl *declaration,
                       const std::string &translation_unit) {
  const auto compiler_id = usr(declaration->getCanonicalDecl());
  if (compiler_id.empty())
    return {};
  const std::string linkage =
      declaration->getFormalLinkage() == clang::Linkage::Internal ? "internal"
                                                                  : "external";
  const auto domain =
      linkage == std::string{"internal"} ? translation_unit : std::string{};
  return history::stable_hash(repository_id.getValue() + "\n" + linkage + "\n" +
                              domain + "\n" + compiler_id);
}

std::string type_reference(clang::QualType type,
                           const std::string &translation_unit) {
  if (type.isNull())
    return {};
  if (const auto *typedef_type = type->getAs<clang::TypedefType>())
    return logical_id(typedef_type->getDecl(), translation_unit);
  if (const auto *tag = type->getAsTagDecl())
    return logical_id(tag, translation_unit);
  return {};
}

history::SourceAnchor anchor(const clang::ASTContext &context,
                             clang::SourceRange range) {
  history::SourceAnchor result;
  const auto &sources = context.getSourceManager();
  const bool macro = range.getBegin().isMacroID() || range.getEnd().isMacroID();
  auto begin = sources.getExpansionLoc(range.getBegin());
  auto end = sources.getExpansionLoc(range.getEnd());
  const auto token_end =
      clang::Lexer::getLocForEndOfToken(end, 0, sources, context.getLangOpts());
  const auto first = sources.getPresumedLoc(begin);
  const auto last =
      sources.getPresumedLoc(token_end.isValid() ? token_end : end);
  if (first.isValid()) {
    result.path = first.getFilename();
    result.begin_line = first.getLine();
    result.begin_column = first.getColumn();
  }
  if (last.isValid()) {
    result.end_line = last.getLine();
    result.end_column = last.getColumn();
  }
  result.role = macro ? "macro_expansion" : "spelling";
  return result;
}

class BodyModel {
public:
  BodyModel(const clang::FunctionDecl *function, std::string translation_unit)
      : translation_unit_(std::move(translation_unit)) {
    for (std::size_t index = 0; index < function->getNumParams(); ++index)
      local_ids_.emplace(function->getParamDecl(index)->getCanonicalDecl(),
                         index);
    next_local_id_ = function->getNumParams();
  }
  std::string build(const clang::Stmt *statement) {
    if (!statement)
      return "empty";
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(statement))
      return build(cast->getSubExpr());
    std::string value = statement->getStmtClassName();
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
      const auto *declaration = reference->getDecl()->getCanonicalDecl();
      if (llvm::isa<clang::ParmVarDecl>(declaration) ||
          (llvm::isa<clang::VarDecl>(declaration) &&
           llvm::cast<clang::VarDecl>(declaration)->isLocalVarDecl())) {
        auto [found, inserted] =
            local_ids_.emplace(declaration, next_local_id_);
        if (inserted)
          ++next_local_id_;
        value += ":local" + std::to_string(found->second);
      } else {
        const auto id = logical_id(reference->getDecl(), translation_unit_);
        value += ":ref:" + id;
        if (!id.empty())
          references_.insert(id);
      }
    } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement)) {
      if (const auto *callee = call->getDirectCallee()) {
        const auto id =
            logical_id(callee->getCanonicalDecl(), translation_unit_);
        value += ":call:" + id;
        if (!id.empty())
          references_.insert(id);
      }
    } else if (const auto *member =
                   llvm::dyn_cast<clang::MemberExpr>(statement)) {
      const auto id = logical_id(member->getMemberDecl(), translation_unit_);
      value += ":member:" + id;
      if (!id.empty())
        references_.insert(id);
    } else if (const auto *constructed =
                   llvm::dyn_cast<clang::CXXConstructExpr>(statement)) {
      const auto id =
          logical_id(constructed->getConstructor(), translation_unit_);
      value += ":construct:" + id;
      if (!id.empty())
        references_.insert(id);
    } else if (const auto *binary =
                   llvm::dyn_cast<clang::BinaryOperator>(statement)) {
      value += ":" + binary->getOpcodeStr().str();
    } else if (const auto *unary =
                   llvm::dyn_cast<clang::UnaryOperator>(statement)) {
      value +=
          ":" + clang::UnaryOperator::getOpcodeStr(unary->getOpcode()).str();
    } else if (const auto *integer =
                   llvm::dyn_cast<clang::IntegerLiteral>(statement)) {
      llvm::SmallString<32> number;
      integer->getValue().toString(number, 10, true);
      value += ":" + std::string(number);
    } else if (const auto *string =
                   llvm::dyn_cast<clang::StringLiteral>(statement)) {
      value += ":string:" + history::stable_hash(string->getString().str());
    }
    value += "[";
    for (const auto *child : statement->children())
      if (child)
        value += build(child) + ";";
    return value + "]";
  }
  std::vector<std::string> references() const {
    return {references_.begin(), references_.end()};
  }

private:
  std::map<const clang::Decl *, std::size_t> local_ids_;
  std::size_t next_local_id_{};
  std::set<std::string> references_;
  std::string translation_unit_;
};

struct State {
  struct MacroUse {
    std::string compiler_id;
    history::SourceAnchor location;
  };
  struct DeclarationSite {
    std::string compiler_id;
    history::SourceAnchor location;
  };
  std::vector<history::ElementSnapshot> elements;
  std::vector<DeclarationSite> declaration_sites;
  std::vector<MacroUse> macro_uses;
};

bool project_owned(const clang::SourceManager &sources,
                   clang::SourceLocation location) {
  const auto expanded = sources.getExpansionLoc(location);
  if (expanded.isInvalid() || sources.isInSystemHeader(expanded))
    return false;
  if (project_root.empty())
    return true;
  const auto filename = sources.getFilename(expanded).str();
  std::error_code error;
  const auto file = std::filesystem::weakly_canonical(filename, error);
  const auto root =
      std::filesystem::weakly_canonical(project_root.getValue(), error);
  if (error)
    return false;
  const auto relative = file.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

class MacroTracker final : public clang::PPCallbacks {
public:
  MacroTracker(clang::Preprocessor &preprocessor, State &state)
      : preprocessor_(preprocessor), sources_(preprocessor.getSourceManager()),
        state_(state) {}

  void MacroDefined(const clang::Token &name,
                    const clang::MacroDirective *directive) override {
    const auto *info = directive ? directive->getMacroInfo() : nullptr;
    if (!info || !project_owned(sources_, info->getDefinitionLoc()))
      return;
    const auto spelling =
        name.getIdentifierInfo()
            ? name.getIdentifierInfo()->getName().str()
            : clang::Lexer::getSpelling(name, sources_,
                                        preprocessor_.getLangOpts());
    const auto location = sources_.getPresumedLoc(
        sources_.getExpansionLoc(info->getDefinitionLoc()));
    std::string shape =
        info->isFunctionLike() ? "function_like\n" : "object_like\n";
    shape += "parameters=" + std::to_string(info->getNumParams()) + "\n";
    for (const auto &token : info->tokens())
      shape += clang::Lexer::getSpelling(token, sources_,
                                         preprocessor_.getLangOpts()) +
               " ";
    history::ElementSnapshot element;
    element.kind = "macro";
    element.qualified_name = spelling;
    element.linkage = "file";
    const auto path = location.isValid()
                          ? normalized_project_path(location.getFilename())
                          : std::string{};
    const auto line = location.isValid() ? location.getLine() : 0U;
    element.compiler_id =
        "macro:" + path + ":" + spelling + ":" + std::to_string(line);
    element.interface_fingerprint = history::stable_hash(
        spelling + "\n" + (info->isFunctionLike() ? "function" : "object") +
        "\n" + std::to_string(info->getNumParams()));
    element.implementation_fingerprint = history::stable_hash(shape);
    element.dependency_fingerprint = history::stable_hash("");
    element.location.path = path;
    element.location.begin_line = line;
    element.location.end_line = line;
    element.location.role = "spelling";
    macro_ids_[info] = element.compiler_id;
    state_.elements.push_back(std::move(element));
  }

  void MacroExpands(const clang::Token &,
                    const clang::MacroDefinition &definition,
                    clang::SourceRange range,
                    const clang::MacroArgs *) override {
    const auto *info = definition.getMacroInfo();
    const auto found = macro_ids_.find(info);
    if (found == macro_ids_.end() || !project_owned(sources_, range.getBegin()))
      return;
    history::SourceAnchor location;
    const auto begin =
        sources_.getPresumedLoc(sources_.getExpansionLoc(range.getBegin()));
    const auto end =
        sources_.getPresumedLoc(sources_.getExpansionLoc(range.getEnd()));
    if (!begin.isValid())
      return;
    location.path = normalized_project_path(begin.getFilename());
    location.begin_line = begin.getLine();
    location.begin_column = begin.getColumn();
    location.end_line = end.isValid() ? end.getLine() : begin.getLine();
    location.end_column = end.isValid() ? end.getColumn() : begin.getColumn();
    location.role = "macro_expansion";
    state_.macro_uses.push_back({found->second, std::move(location)});
  }

private:
  clang::Preprocessor &preprocessor_;
  clang::SourceManager &sources_;
  State &state_;
  std::map<const clang::MacroInfo *, std::string> macro_ids_;
};

class Visitor final : public clang::RecursiveASTVisitor<Visitor> {
public:
  Visitor(clang::ASTContext &context, State &state)
      : context_(context), state_(state) {}
  bool VisitFunctionDecl(clang::FunctionDecl *declaration) {
    const auto &sources = context_.getSourceManager();
    const auto location = sources.getExpansionLoc(declaration->getLocation());
    const auto filename = sources.getFilename(location).str();
    if (declaration->isImplicit() ||
        !project_owned(sources, declaration->getLocation()))
      return true;
    if (!declaration->isThisDeclarationADefinition()) {
      auto site = anchor(context_, declaration->getSourceRange());
      site.role = "declaration";
      state_.declaration_sites.push_back(
          {usr(declaration->getCanonicalDecl()), std::move(site)});
      return true;
    }
    if (!seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    const bool method = llvm::isa<clang::CXXMethodDecl>(declaration);
    const bool templated =
        declaration->getDescribedFunctionTemplate() != nullptr;
    element.kind = templated
                       ? (method ? "method_template" : "function_template")
                       : (method ? "method" : "function");
    if (declaration->getTemplateSpecializationKind() != clang::TSK_Undeclared)
      element.kind = method ? "method_template_specialization"
                            : "function_template_specialization";
    if (const auto *method_decl =
            llvm::dyn_cast<clang::CXXMethodDecl>(declaration))
      element.parent_element_id = logical_id(method_decl->getParent(),
                                             normalized_project_path(filename));
    element.qualified_name = declaration->getQualifiedNameAsString();
    element.linkage =
        declaration->getFormalLinkage() == clang::Linkage::Internal
            ? "internal"
            : "external";
    const auto interface_shape =
        element.kind + ":" +
        declaration->getType().getCanonicalType().getAsString(
            context_.getPrintingPolicy());
    element.interface_fingerprint = history::stable_hash(interface_shape);
    const auto main_location =
        sources.getLocForStartOfFile(sources.getMainFileID());
    BodyModel body(declaration, normalized_project_path(
                                    sources.getFilename(main_location).str()));
    element.implementation_fingerprint =
        history::stable_hash(body.build(declaration->getBody()));
    element.referenced_compiler_ids = body.references();
    const auto translation_unit =
        normalized_project_path(sources.getFilename(main_location).str());
    const auto add_type = [&](clang::QualType type) {
      const auto id = type_reference(type, translation_unit);
      if (!id.empty())
        element.referenced_compiler_ids.push_back(id);
    };
    add_type(declaration->getReturnType());
    for (const auto *parameter : declaration->parameters())
      add_type(parameter->getType());
    std::sort(element.referenced_compiler_ids.begin(),
              element.referenced_compiler_ids.end());
    element.referenced_compiler_ids.erase(
        std::unique(element.referenced_compiler_ids.begin(),
                    element.referenced_compiler_ids.end()),
        element.referenced_compiler_ids.end());
    std::string dependencies;
    for (const auto &id : element.referenced_compiler_ids)
      dependencies += id + "\n";
    element.dependency_fingerprint = history::stable_hash(dependencies);
    element.location = anchor(context_, declaration->getSourceRange());
    element.location.role = "definition";
    state_.elements.push_back(std::move(element));
    return true;
  }

  bool VisitRecordDecl(clang::RecordDecl *declaration) {
    if (!declaration->isCompleteDefinition() || declaration->isImplicit() ||
        !project_owned(context_.getSourceManager(),
                       declaration->getLocation()) ||
        !seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    if (llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(declaration))
      element.kind = "record_template_partial_specialization";
    else if (llvm::isa<clang::ClassTemplateSpecializationDecl>(declaration))
      element.kind = "record_template_specialization";
    else
      element.kind =
          declaration->getDescribedTemplate() ? "record_template" : "record";
    element.qualified_name = declaration->getQualifiedNameAsString();
    element.interface_fingerprint = history::stable_hash(
        element.kind + "\n" + declaration->getKindName().str() + "\n" +
        element.qualified_name);
    std::string members;
    const auto main_location = context_.getSourceManager().getLocForStartOfFile(
        context_.getSourceManager().getMainFileID());
    const auto translation_unit = normalized_project_path(
        context_.getSourceManager().getFilename(main_location).str());
    for (const auto *field : declaration->fields()) {
      members += field->getNameAsString() + ":" +
                 field->getType().getCanonicalType().getAsString() + "\n";
      const auto id = type_reference(field->getType(), translation_unit);
      if (!id.empty())
        element.referenced_compiler_ids.push_back(id);
    }
    if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration))
      for (const auto &base : record->bases()) {
        const auto id = type_reference(base.getType(), translation_unit);
        if (!id.empty())
          element.referenced_compiler_ids.push_back(id);
      }
    std::sort(element.referenced_compiler_ids.begin(),
              element.referenced_compiler_ids.end());
    element.referenced_compiler_ids.erase(
        std::unique(element.referenced_compiler_ids.begin(),
                    element.referenced_compiler_ids.end()),
        element.referenced_compiler_ids.end());
    element.implementation_fingerprint = history::stable_hash(members);
    element.dependency_fingerprint = history::stable_hash(
        nlohmann::json(element.referenced_compiler_ids).dump());
    element.location = anchor(context_, declaration->getSourceRange());
    state_.elements.push_back(std::move(element));
    return true;
  }

  bool VisitFieldDecl(clang::FieldDecl *declaration) {
    if (declaration->isImplicit() ||
        !project_owned(context_.getSourceManager(),
                       declaration->getLocation()) ||
        !seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    element.kind = "field";
    const auto main_location = context_.getSourceManager().getLocForStartOfFile(
        context_.getSourceManager().getMainFileID());
    element.parent_element_id = logical_id(
        declaration->getParent(),
        normalized_project_path(
            context_.getSourceManager().getFilename(main_location).str()));
    element.qualified_name = declaration->getQualifiedNameAsString();
    element.interface_fingerprint = history::stable_hash(
        declaration->getType().getCanonicalType().getAsString());
    const auto id = type_reference(
        declaration->getType(),
        normalized_project_path(
            context_.getSourceManager().getFilename(main_location).str()));
    if (!id.empty())
      element.referenced_compiler_ids.push_back(id);
    element.implementation_fingerprint = history::stable_hash(
        declaration->hasInClassInitializer() ? "has_initializer"
                                             : "no_initializer");
    element.dependency_fingerprint = history::stable_hash(id);
    element.location = anchor(context_, declaration->getSourceRange());
    state_.elements.push_back(std::move(element));
    return true;
  }

  bool VisitEnumDecl(clang::EnumDecl *declaration) {
    if (!declaration->isCompleteDefinition() ||
        !project_owned(context_.getSourceManager(),
                       declaration->getLocation()) ||
        !seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    element.kind = "enum";
    element.qualified_name = declaration->getQualifiedNameAsString();
    element.interface_fingerprint = history::stable_hash(
        declaration->getIntegerType().getCanonicalType().getAsString());
    std::string values;
    for (const auto *constant : declaration->enumerators()) {
      llvm::SmallString<32> number;
      constant->getInitVal().toString(number, 10);
      values += constant->getNameAsString() + "=" + std::string(number) + "\n";
    }
    element.implementation_fingerprint = history::stable_hash(values);
    element.dependency_fingerprint = history::stable_hash("");
    element.location = anchor(context_, declaration->getSourceRange());
    state_.elements.push_back(std::move(element));
    return true;
  }

  bool VisitEnumConstantDecl(clang::EnumConstantDecl *declaration) {
    if (!project_owned(context_.getSourceManager(),
                       declaration->getLocation()) ||
        !seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    element.kind = "enum_constant";
    element.qualified_name = declaration->getQualifiedNameAsString();
    const auto main_location = context_.getSourceManager().getLocForStartOfFile(
        context_.getSourceManager().getMainFileID());
    element.parent_element_id = logical_id(
        llvm::cast<clang::EnumDecl>(declaration->getDeclContext()),
        normalized_project_path(
            context_.getSourceManager().getFilename(main_location).str()));
    element.interface_fingerprint = history::stable_hash("enum_constant");
    llvm::SmallString<32> number;
    declaration->getInitVal().toString(number, 10);
    element.implementation_fingerprint =
        history::stable_hash(std::string(number));
    element.dependency_fingerprint = history::stable_hash("");
    element.location = anchor(context_, declaration->getSourceRange());
    state_.elements.push_back(std::move(element));
    return true;
  }

  bool VisitTypedefNameDecl(clang::TypedefNameDecl *declaration) {
    if (!project_owned(context_.getSourceManager(),
                       declaration->getLocation()) ||
        !seen_.insert(declaration->getCanonicalDecl()).second)
      return true;
    history::ElementSnapshot element;
    element.compiler_id = usr(declaration->getCanonicalDecl());
    element.kind =
        llvm::isa<clang::TypeAliasDecl>(declaration) ? "type_alias" : "typedef";
    element.qualified_name = declaration->getQualifiedNameAsString();
    element.interface_fingerprint = history::stable_hash(
        declaration->getUnderlyingType().getCanonicalType().getAsString());
    element.implementation_fingerprint = history::stable_hash("");
    element.dependency_fingerprint = history::stable_hash("");
    element.location = anchor(context_, declaration->getSourceRange());
    state_.elements.push_back(std::move(element));
    return true;
  }

private:
  clang::ASTContext &context_;
  State &state_;
  std::set<const clang::Decl *> seen_;
};

class Consumer final : public clang::ASTConsumer {
public:
  explicit Consumer(State &state) : state_(state) {}
  void HandleTranslationUnit(clang::ASTContext &context) override {
    Visitor(context, state_).TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  State &state_;
};

class Action final : public clang::ASTFrontendAction {
public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef) override {
    compiler.getPreprocessor().addPPCallbacks(
        std::make_unique<MacroTracker>(compiler.getPreprocessor(), state_));
    return std::make_unique<Consumer>(state_);
  }
  void EndSourceFileAction() override {
    history::TuManifest manifest;
    manifest.repository_id = repository_id;
    manifest.source_revision = revision;
    manifest.translation_unit = normalized_project_path(getCurrentFile().str());
    manifest.source_blob = source_blob;
    manifest.context_id = context_fingerprint;
    manifest.configuration = configuration;
    manifest.build_variant = nlohmann::json::parse(build_variant.getValue())
                                 .get<history::BuildVariant>();
    const auto clang_version = clang::getClangFullVersion();
    manifest.extractor_fingerprint =
        "repotraverse-" + std::string(history::build::kToolVersion) +
        ";llvm-" LLVM_VERSION_STRING + ";clang-" + clang_version + ";build-" +
        std::string(history::build::kBuildMode);
    manifest.producer.tool_version = std::string(history::build::kToolVersion);
    manifest.producer.llvm_version = LLVM_VERSION_STRING;
    manifest.producer.clang_version = clang_version;
    manifest.producer.build_mode = std::string(history::build::kBuildMode);
    manifest.producer.host_architecture =
        std::string(history::build::kHostArchitecture);
    manifest.coverage.capabilities = {"logical_elements",
                                      "semantic_variants",
                                      "tu_observations",
                                      "binding_normalized_bodies",
                                      "resolved_dependencies",
                                      "types",
                                      "fields",
                                      "templates",
                                      "macros"};
    if (getCompilerInstance().getDiagnostics().hasErrorOccurred()) {
      manifest.coverage.status = "partial";
      manifest.coverage.gaps.push_back("compiler diagnostics contain errors");
    }
    std::sort(state_.elements.begin(), state_.elements.end(),
              [](const auto &a, const auto &b) {
                return std::tie(a.qualified_name, a.compiler_id) <
                       std::tie(b.qualified_name, b.compiler_id);
              });
    for (const auto &snapshot : state_.elements) {
      history::LogicalElement element;
      element.compiler_id = snapshot.compiler_id;
      element.repository_id = manifest.repository_id;
      element.parent_element_id = snapshot.parent_element_id;
      element.kind = snapshot.kind;
      element.qualified_name = snapshot.qualified_name;
      element.owner_file = normalized_project_path(snapshot.location.path);
      element.linkage = snapshot.linkage;
      const auto domain = element.linkage == "internal"
                              ? manifest.translation_unit
                              : std::string{};
      element.element_id =
          history::stable_hash(manifest.repository_id + "\n" + element.linkage +
                               "\n" + domain + "\n" + element.compiler_id);
      history::SemanticVariant variant;
      variant.element_id = element.element_id;
      variant.interface_fingerprint = snapshot.interface_fingerprint;
      variant.implementation_fingerprint = snapshot.implementation_fingerprint;
      variant.dependency_fingerprint = snapshot.dependency_fingerprint;
      variant.referenced_element_ids = snapshot.referenced_compiler_ids;
      variant.variant_id = history::stable_hash(
          variant.element_id + "\n" + variant.interface_fingerprint + "\n" +
          variant.implementation_fingerprint + "\n" +
          variant.dependency_fingerprint);
      manifest.elements.push_back(element);
      manifest.variants.push_back(variant);
      auto location = snapshot.location;
      location.path = element.owner_file;
      manifest.observations.push_back(
          {element.element_id, variant.variant_id, std::move(location)});
    }
    std::map<std::string, std::string> element_by_compiler;
    std::map<std::string, std::string> variant_by_element;
    for (const auto &element : manifest.elements)
      element_by_compiler[element.compiler_id] = element.element_id;
    for (auto &variant : manifest.variants) {
      std::vector<std::string> resolved;
      for (const auto &reference : variant.referenced_element_ids)
        if (const auto found = element_by_compiler.find(reference);
            found != element_by_compiler.end())
          resolved.push_back(found->second);
      std::sort(resolved.begin(), resolved.end());
      resolved.erase(std::unique(resolved.begin(), resolved.end()),
                     resolved.end());
      variant.referenced_element_ids = std::move(resolved);
    }
    for (const auto &variant : manifest.variants)
      variant_by_element[variant.element_id] = variant.variant_id;
    std::set<std::tuple<std::string, std::string, std::uint32_t, std::uint32_t>>
        observed_locations;
    for (const auto &observation : manifest.observations)
      observed_locations.emplace(
          observation.element_id, observation.location.path,
          observation.location.begin_line, observation.location.begin_column);
    for (auto &site : state_.declaration_sites) {
      const auto element = element_by_compiler.find(site.compiler_id);
      if (element == element_by_compiler.end())
        continue;
      site.location.path = normalized_project_path(site.location.path);
      if (!observed_locations
               .emplace(element->second, site.location.path,
                        site.location.begin_line, site.location.begin_column)
               .second)
        continue;
      manifest.observations.push_back({element->second,
                                       variant_by_element.at(element->second),
                                       std::move(site.location)});
    }
    for (const auto &use : state_.macro_uses) {
      const auto macro = element_by_compiler.find(use.compiler_id);
      if (macro == element_by_compiler.end())
        continue;
      std::string containing;
      std::uint64_t best_span = std::numeric_limits<std::uint64_t>::max();
      for (const auto &observation : manifest.observations) {
        if (observation.element_id == macro->second ||
            observation.location.path != use.location.path ||
            observation.location.begin_line > use.location.begin_line ||
            observation.location.end_line < use.location.end_line)
          continue;
        const auto span = static_cast<std::uint64_t>(
            observation.location.end_line - observation.location.begin_line);
        if (span < best_span) {
          best_span = span;
          containing = observation.element_id;
        }
      }
      manifest.macro_expansions.push_back(
          {macro->second, containing, use.location});
    }
    nlohmann::json identity = {{"repository", manifest.repository_id},
                               {"revision", manifest.source_revision},
                               {"configuration", manifest.configuration},
                               {"build_variant", manifest.build_variant},
                               {"tu", manifest.translation_unit},
                               {"blob", manifest.source_blob},
                               {"context", manifest.context_id},
                               {"observations", manifest.observations},
                               {"macro_expansions", manifest.macro_expansions}};
    manifest.manifest_id = history::stable_hash(identity.dump());
    llvm::outs() << history::canonical_json(nlohmann::json(manifest));
  }

private:
  State state_;
};
class Factory final : public clang::tooling::FrontendActionFactory {
public:
  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<Action>();
  }
};
} // namespace

int main(int argc, const char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    llvm::outs() << "repotraverse clang-extractor "
                 << history::build::kToolVersion << " (LLVM/Clang "
                 << LLVM_VERSION_STRING << "/" << clang::getClangFullVersion()
                 << "; build mode: " << history::build::kBuildMode << ")\n";
    return 0;
  }
  auto parser =
      clang::tooling::CommonOptionsParser::create(argc, argv, category);
  if (!parser) {
    llvm::errs() << llvm::toString(parser.takeError()) << '\n';
    return 2;
  }
  clang::tooling::ClangTool tool(parser->getCompilations(),
                                 parser->getSourcePathList());
  Factory factory;
  return tool.run(&factory);
}
