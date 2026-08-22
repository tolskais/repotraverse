#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
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
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "history/ir.hpp"

namespace {
llvm::cl::OptionCategory category("element history extractor");
llvm::cl::opt<std::string> revision("source-revision", llvm::cl::init("working-tree"), llvm::cl::cat(category));
llvm::cl::opt<std::string> configuration("configuration", llvm::cl::init("default"), llvm::cl::cat(category));
llvm::cl::opt<std::string> context_fingerprint("context-fingerprint", llvm::cl::init("unspecified"), llvm::cl::cat(category));

std::string usr(const clang::Decl* declaration) {
    llvm::SmallString<128> value;
    return clang::index::generateUSRForDecl(declaration, value) ? std::string{} : std::string(value);
}

history::SourceAnchor anchor(const clang::ASTContext& context, clang::SourceRange range) {
    history::SourceAnchor result;
    const auto& sources = context.getSourceManager();
    const bool macro = range.getBegin().isMacroID() || range.getEnd().isMacroID();
    auto begin = sources.getExpansionLoc(range.getBegin());
    auto end = sources.getExpansionLoc(range.getEnd());
    const auto token_end = clang::Lexer::getLocForEndOfToken(end, 0, sources, context.getLangOpts());
    const auto first = sources.getPresumedLoc(begin);
    const auto last = sources.getPresumedLoc(token_end.isValid() ? token_end : end);
    if (first.isValid()) {
        result.path = first.getFilename(); result.begin_line = first.getLine();
        result.begin_column = first.getColumn();
    }
    if (last.isValid()) { result.end_line = last.getLine(); result.end_column = last.getColumn(); }
    result.role = macro ? "macro_expansion" : "spelling";
    return result;
}

class BodyModel {
public:
    explicit BodyModel(const clang::FunctionDecl* function) {
        for (std::size_t index = 0; index < function->getNumParams(); ++index)
            local_ids_.emplace(function->getParamDecl(index)->getCanonicalDecl(), index);
        next_local_id_ = function->getNumParams();
    }
    std::string build(const clang::Stmt* statement) {
        if (!statement) return "empty";
        if (const auto* cast = llvm::dyn_cast<clang::ImplicitCastExpr>(statement)) return build(cast->getSubExpr());
        std::string value = statement->getStmtClassName();
        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
            const auto* declaration = reference->getDecl()->getCanonicalDecl();
            if (llvm::isa<clang::ParmVarDecl>(declaration) ||
                (llvm::isa<clang::VarDecl>(declaration) &&
                 llvm::cast<clang::VarDecl>(declaration)->isLocalVarDecl())) {
                auto [found, inserted] = local_ids_.emplace(declaration, next_local_id_);
                if (inserted) ++next_local_id_;
                value += ":local" + std::to_string(found->second);
            } else {
                const auto id = usr(declaration); value += ":ref:" + id;
                if (!id.empty()) references_.insert(id);
            }
        } else if (const auto* call = llvm::dyn_cast<clang::CallExpr>(statement)) {
            if (const auto* callee = call->getDirectCallee()) {
                const auto id = usr(callee->getCanonicalDecl()); value += ":call:" + id;
                if (!id.empty()) references_.insert(id);
            }
        } else if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(statement)) {
            value += ":" + binary->getOpcodeStr().str();
        } else if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(statement)) {
            value += ":" + clang::UnaryOperator::getOpcodeStr(unary->getOpcode()).str();
        } else if (const auto* integer = llvm::dyn_cast<clang::IntegerLiteral>(statement)) {
            llvm::SmallString<32> number; integer->getValue().toString(number, 10, true);
            value += ":" + std::string(number);
        } else if (const auto* string = llvm::dyn_cast<clang::StringLiteral>(statement)) {
            value += ":string:" + history::stable_hash(string->getString().str());
        }
        value += "[";
        for (const auto* child : statement->children()) if (child) value += build(child) + ";";
        return value + "]";
    }
    std::vector<std::string> references() const { return {references_.begin(), references_.end()}; }
private:
    std::map<const clang::Decl*, std::size_t> local_ids_;
    std::size_t next_local_id_{};
    std::set<std::string> references_;
};

struct State { std::vector<history::ElementSnapshot> elements; };

class Visitor final : public clang::RecursiveASTVisitor<Visitor> {
public:
    Visitor(clang::ASTContext& context, State& state) : context_(context), state_(state) {}
    bool VisitFunctionDecl(clang::FunctionDecl* declaration) {
        const auto& sources = context_.getSourceManager();
        if (!declaration->isThisDeclarationADefinition() || declaration->isImplicit() ||
            !sources.isWrittenInMainFile(sources.getExpansionLoc(declaration->getLocation())) ||
            !seen_.insert(declaration->getCanonicalDecl()).second) return true;
        history::ElementSnapshot element;
        element.compiler_id = usr(declaration->getCanonicalDecl());
        element.kind = llvm::isa<clang::CXXMethodDecl>(declaration) ? "method" : "function";
        element.qualified_name = declaration->getQualifiedNameAsString();
        const auto interface_shape = element.kind + ":" +
            declaration->getType().getCanonicalType().getAsString(context_.getPrintingPolicy());
        element.interface_fingerprint = history::stable_hash(interface_shape);
        BodyModel body(declaration);
        element.implementation_fingerprint = history::stable_hash(body.build(declaration->getBody()));
        element.referenced_compiler_ids = body.references();
        std::string dependencies;
        for (const auto& id : element.referenced_compiler_ids) dependencies += id + "\n";
        element.dependency_fingerprint = history::stable_hash(dependencies);
        element.location = anchor(context_, declaration->getSourceRange());
        state_.elements.push_back(std::move(element));
        return true;
    }
private:
    clang::ASTContext& context_; State& state_; std::set<const clang::Decl*> seen_;
};

class Consumer final : public clang::ASTConsumer {
public:
    explicit Consumer(State& state) : state_(state) {}
    void HandleTranslationUnit(clang::ASTContext& context) override { Visitor(context, state_).TraverseDecl(context.getTranslationUnitDecl()); }
private: State& state_;
};

class Action final : public clang::ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<Consumer>(state_);
    }
    void EndSourceFileAction() override {
        history::EvidenceBundle bundle;
        bundle.source_revision = revision; bundle.configuration = configuration;
        bundle.context_fingerprint = context_fingerprint;
        bundle.extractor_fingerprint = "clang-" + clang::getClangFullVersion();
        bundle.coverage.capabilities = {"function_snapshots", "binding_normalized_bodies", "resolved_dependencies"};
        if (getCompilerInstance().getDiagnostics().hasErrorOccurred()) {
            bundle.coverage.status = "partial"; bundle.coverage.gaps.push_back("compiler diagnostics contain errors");
        }
        std::sort(state_.elements.begin(), state_.elements.end(), [](const auto& a, const auto& b) {
            return std::tie(a.qualified_name, a.compiler_id) < std::tie(b.qualified_name, b.compiler_id);
        });
        bundle.elements = std::move(state_.elements);
        llvm::outs() << history::canonical_json(nlohmann::json(bundle));
    }
private: State state_;
};
class Factory final : public clang::tooling::FrontendActionFactory {
public: std::unique_ptr<clang::FrontendAction> create() override { return std::make_unique<Action>(); }
};
}  // namespace

int main(int argc, const char** argv) {
    auto parser = clang::tooling::CommonOptionsParser::create(argc, argv, category);
    if (!parser) { llvm::errs() << llvm::toString(parser.takeError()) << '\n'; return 2; }
    clang::tooling::ClangTool tool(parser->getCompilations(), parser->getSourcePathList());
    Factory factory; return tool.run(&factory);
}
