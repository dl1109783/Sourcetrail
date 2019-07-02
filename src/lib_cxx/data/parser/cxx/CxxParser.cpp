#include "CxxParser.h"

#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Support/TargetSelect.h>

#include "ApplicationSettings.h"
#include "ASTAction.h"
#include "CanonicalFilePathCache.h"
#include "CxxCompilationDatabaseSingle.h"
#include "CxxDiagnosticConsumer.h"
#include "FilePath.h"
#include "FileRegister.h"
#include "IndexerCommandCxx.h"
#include "logging.h"
#include "ParserClient.h"
#include "ResourcePaths.h"
#include "SingleFrontendActionFactory.h"
#include "TextAccess.h"
#include "utilityString.h"
#include "utility.h"

namespace
{
	struct ClangInvocationInfo
	{
		std::string invocation;
		std::string errors;
	};

	// copied from clang codebase
	clang::driver::Driver *newDriver(
		clang::DiagnosticsEngine *Diagnostics, const char *BinaryName,
		clang::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
		clang::driver::Driver *CompilerDriver =
			new clang::driver::Driver(BinaryName, llvm::sys::getDefaultTargetTriple(),
				*Diagnostics, std::move(VFS));
		CompilerDriver->setTitle("clang_based_tool");
		return CompilerDriver;
	}

	// copied and stitched together from clang codebase
	ClangInvocationInfo getClangInvocationString(const clang::tooling::CompilationDatabase* compilationDatabase)
	{
		ClangInvocationInfo invocationInfo;

		if (!compilationDatabase->getAllCompileCommands().empty())
		{
			std::vector<std::string> CommandLine = compilationDatabase->getAllCompileCommands().front().CommandLine;

			std::vector<const char*> Argv;
			for (const std::string &Str : CommandLine)
				Argv.push_back(Str.c_str());
			const char *const BinaryName = Argv[0];
			clang::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts = new clang::DiagnosticOptions();
			unsigned MissingArgIndex, MissingArgCount;
			std::unique_ptr<llvm::opt::OptTable> Opts = clang::driver::createDriverOptTable();
			llvm::opt::InputArgList ParsedArgs = Opts->ParseArgs(
				clang::ArrayRef<const char *>(Argv).slice(1), MissingArgIndex, MissingArgCount);
			clang::ParseDiagnosticArgs(*DiagOpts, ParsedArgs);

			llvm::raw_string_ostream diagnosticsStream(invocationInfo.errors);
			clang::TextDiagnosticPrinter DiagnosticPrinter(
				diagnosticsStream, &*DiagOpts);
			clang::DiagnosticsEngine Diagnostics(
				clang::IntrusiveRefCntPtr<clang::DiagnosticIDs>(new clang::DiagnosticIDs()), &*DiagOpts,
				&DiagnosticPrinter, false);

			llvm::IntrusiveRefCntPtr<clang::FileManager> Files(new clang::FileManager(clang::FileSystemOptions()));

			const std::unique_ptr<clang::driver::Driver> Driver(
				newDriver(&Diagnostics, BinaryName, Files->getVirtualFileSystem()));
			// Since the input might only be virtual, don't check whether it exists.
			Driver->setCheckInputsExist(false);
			const std::unique_ptr<clang::driver::Compilation> Compilation(
				Driver->BuildCompilation(llvm::makeArrayRef(Argv)));

			if (Compilation)
			{
				llvm::raw_string_ostream ss(invocationInfo.invocation);
				Compilation->getJobs().Print(ss, "", true);
				ss.flush();
			}

			diagnosticsStream.flush();

			invocationInfo.invocation = utility::trim(invocationInfo.invocation);
			invocationInfo.errors = utility::trim(invocationInfo.errors);
		}
		return invocationInfo;
	}

	std::vector<std::string> prependSyntaxOnlyToolArgs(const std::vector<std::string>& args)
	{
		return utility::concat(std::vector<std::string>({ "clang-tool", "-fsyntax-only" }), args);
	}

	std::vector<std::string> appendFilePath(const std::vector<std::string>& args, llvm::StringRef filePath)
	{
		return utility::concat(args, { filePath.str() });
	}

	// custom implementation of clang::runToolOnCodeWithArgs which also sets our custon DiagnosticConsumer
	bool runToolOnCodeWithArgs(
		clang::DiagnosticConsumer* DiagConsumer,
		clang::FrontendAction *ToolAction,
		const llvm::Twine &Code,
		const std::vector<std::string> &Args,
		const llvm::Twine &FileName = "input.cc",
		const clang::tooling::FileContentMappings &VirtualMappedFiles = clang::tooling::FileContentMappings()
	)
	{
		CxxParser::initializeLLVM();

		llvm::SmallString<16> FileNameStorage;
		llvm::StringRef FileNameRef = FileName.toNullTerminatedStringRef(FileNameStorage);

		llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFileSystem(new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
		llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(new llvm::vfs::InMemoryFileSystem);
		OverlayFileSystem->pushOverlay(InMemoryFileSystem);
		llvm::IntrusiveRefCntPtr<clang::FileManager> Files(new clang::FileManager(clang::FileSystemOptions(), OverlayFileSystem));

		clang::tooling::ToolInvocation Invocation(prependSyntaxOnlyToolArgs(appendFilePath(Args, FileNameRef)),	ToolAction, Files.get());

		llvm::SmallString<1024> CodeStorage;
		llvm::StringRef CodeRef = Code.toNullTerminatedStringRef(CodeStorage);

		InMemoryFileSystem->addFile(FileNameRef, 0,
			llvm::MemoryBuffer::getMemBufferCopy(CodeRef));

		Invocation.setDiagnosticConsumer(DiagConsumer);

		return Invocation.run();
	}
}

std::vector<std::string> CxxParser::getCommandlineArgumentsEssential(const std::vector<std::wstring>& compilerFlags)
{
	std::vector<std::string> args;

	// The option -fno-delayed-template-parsing signals that templates that there should
	// be AST elements for unused template functions as well.
	args.push_back("-fno-delayed-template-parsing");

	// The option -fexceptions signals that clang should watch out for exception-related code during indexing.
	args.push_back("-fexceptions");

	// The option -c signals that no executable is built.
	args.push_back("-c");

	// The option -w disables all warnings.
	args.push_back("-w");

	// This option tells clang just to continue parsing no matter how manny errors have been thrown.
	args.push_back("-ferror-limit=0");

	for (const std::wstring& compilerFlag : compilerFlags)
	{
		args.push_back(utility::encodeToUtf8(compilerFlag));
	}

	return args;
}

void CxxParser::initializeLLVM()
{
	static bool intialized = false;
	if (!intialized)
	{
		llvm::InitializeAllTargets();
		llvm::InitializeAllTargetMCs();
		llvm::InitializeAllAsmPrinters();
		llvm::InitializeAllAsmParsers();
		intialized = true;
	}
}

CxxParser::CxxParser(
	std::shared_ptr<ParserClient> client,
	std::shared_ptr<FileRegister> fileRegister,
	std::shared_ptr<IndexerStateInfo> indexerStateInfo
)
	: Parser(client)
	, m_fileRegister(fileRegister)
	, m_indexerStateInfo(indexerStateInfo)
{
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmParser();
}

void CxxParser::buildIndex(std::shared_ptr<IndexerCommandCxx> indexerCommand)
{
	clang::tooling::CompileCommand compileCommand;
	compileCommand.Filename = utility::encodeToUtf8(indexerCommand->getSourceFilePath().wstr());
	compileCommand.Directory = utility::encodeToUtf8(indexerCommand->getWorkingDirectory().wstr());
	std::vector<std::wstring> args = indexerCommand->getCompilerFlags();
	if (!args.empty() && !utility::isPrefix<std::wstring>(L"-", args.front()))
	{
		args.erase(args.begin());
	}
	compileCommand.CommandLine = getCommandlineArgumentsEssential(args);
	compileCommand.CommandLine = prependSyntaxOnlyToolArgs(compileCommand.CommandLine);

	CxxCompilationDatabaseSingle compilationDatabase(compileCommand);
	runTool(&compilationDatabase, indexerCommand->getSourceFilePath());
}

void CxxParser::buildIndex(const std::wstring& fileName, std::shared_ptr<TextAccess> fileContent, std::vector<std::wstring> compilerFlags)
{
	std::shared_ptr<CanonicalFilePathCache> canonicalFilePathCache =
		std::make_shared<CanonicalFilePathCache>(m_fileRegister);

	std::shared_ptr<CxxDiagnosticConsumer> diagnostics = getDiagnostics(FilePath(), canonicalFilePathCache, false);
	clang::ASTFrontendAction* action = new ASTAction(m_client, canonicalFilePathCache, m_indexerStateInfo);

	std::vector<std::string> args = getCommandlineArgumentsEssential(compilerFlags);

	runToolOnCodeWithArgs(
		diagnostics.get(),
		action,
		fileContent->getText(),
		args,
		utility::encodeToUtf8(fileName)
	);
}

void CxxParser::runTool(clang::tooling::CompilationDatabase* compilationDatabase, const FilePath& sourceFilePath)
{
	initializeLLVM();

	clang::tooling::ClangTool tool(*compilationDatabase, std::vector<std::string>(1, utility::encodeToUtf8(sourceFilePath.wstr())));

	std::shared_ptr<CanonicalFilePathCache> canonicalFilePathCache = std::make_shared<CanonicalFilePathCache>(m_fileRegister);

	std::shared_ptr<CxxDiagnosticConsumer> diagnostics = getDiagnostics(sourceFilePath, canonicalFilePathCache, true);

	tool.setDiagnosticConsumer(diagnostics.get());

	ClangInvocationInfo info;
	if (LogManager::getInstance()->getLoggingEnabled())
	{
		info = getClangInvocationString(compilationDatabase);
		LOG_INFO("Clang Invocation: " + info.invocation.substr(0, ApplicationSettings::getInstance()->getVerboseIndexerLoggingEnabled() ? std::string::npos : 20000));

		if (!info.errors.empty())
		{
			LOG_INFO("Clang Invocation errors: " + info.errors);
		}
	}

	clang::ASTFrontendAction* action = new ASTAction(m_client, canonicalFilePathCache, m_indexerStateInfo);
	tool.run(new SingleFrontendActionFactory(action));

	if (!m_client->hasContent())
	{
		if (info.invocation.empty())
		{
			info = getClangInvocationString(compilationDatabase);
		}

		if (!info.errors.empty())
		{
			Id fileId = m_client->recordFile(sourceFilePath, true);
			m_client->recordError(
				L"Clang Invocation errors: " + utility::decodeFromUtf8(info.errors),
				true,
				true,
				sourceFilePath,
				ParseLocation(fileId, 1, 1)
			);
		}
	}
}

std::shared_ptr<CxxDiagnosticConsumer> CxxParser::getDiagnostics(const FilePath& sourceFilePath, std::shared_ptr<CanonicalFilePathCache> canonicalFilePathCache, bool logErrors) const
{
	llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> options = new clang::DiagnosticOptions();
	return std::make_shared<CxxDiagnosticConsumer>(
		llvm::errs(), &*options, m_client, canonicalFilePathCache, sourceFilePath, logErrors
	);
}
