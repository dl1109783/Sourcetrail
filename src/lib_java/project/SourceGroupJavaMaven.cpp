#include "SourceGroupJavaMaven.h"

#include "DialogView.h"
#include "ApplicationSettings.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "FileSystem.h"
#include "MessageStatus.h"
#include "ScopedFunctor.h"
#include "utility.h"
#include "utilityJava.h"
#include "utilityMaven.h"
#include "Application.h"

SourceGroupJavaMaven::SourceGroupJavaMaven(std::shared_ptr<SourceGroupSettingsJavaMaven> settings)
	: m_settings(settings)
{
}

bool SourceGroupJavaMaven::prepareIndexing()
{
	if (!utility::prepareJavaEnvironmentAndDisplayOccurringErrors())
	{
		return false;
	}

	if (!prepareMavenData())
	{
		return false;
	}

	return true;
}

std::vector<FilePath> SourceGroupJavaMaven::getAllSourcePaths() const
{
	std::vector<FilePath> sourcePaths;
	if (m_settings && m_settings->getMavenProjectFilePathExpandedAndAbsolute().exists())
	{
		std::shared_ptr<DialogView> dialogView = Application::getInstance()->getDialogView(DialogView::UseCase::PROJECT_SETUP);
		dialogView->showUnknownProgressDialog(L"Preparing Project", L"Maven\nFetching Source Directories");

		const FilePath mavenPath(ApplicationSettings::getInstance()->getMavenPath());
		const FilePath projectRootPath = m_settings->getMavenProjectFilePathExpandedAndAbsolute().getParentDirectory();
		sourcePaths = utility::mavenGetAllDirectoriesFromEffectivePom(mavenPath, projectRootPath, m_settings->getShouldIndexMavenTests());

		dialogView->hideUnknownProgressDialog();
	}
	return sourcePaths;
}

std::vector<FilePath> SourceGroupJavaMaven::doGetClassPath() const
{
	std::vector<FilePath> classPath = utility::getClassPath({}, true, getAllSourceFilePaths());

	if (m_settings && m_settings->getMavenDependenciesDirectoryPath().exists())
	{
		std::vector<FilePath> mavenJarPaths = FileSystem::getFilePathsFromDirectory(
			m_settings->getMavenDependenciesDirectoryPath(),
			{ L".jar" }
		);

		for (const FilePath& mavenJarPath : mavenJarPaths)
		{
			LOG_INFO(L"Adding jar to classpath: " + mavenJarPath.wstr());
		}

		utility::append(classPath, mavenJarPaths);
	}

	return classPath;
}

std::shared_ptr<SourceGroupSettingsJava> SourceGroupJavaMaven::getSourceGroupSettingsJava()
{
	return m_settings;
}

std::shared_ptr<const SourceGroupSettingsJava> SourceGroupJavaMaven::getSourceGroupSettingsJava() const
{
	return m_settings;
}

bool SourceGroupJavaMaven::prepareMavenData()
{
	if (m_settings && m_settings->getMavenProjectFilePathExpandedAndAbsolute().exists())
	{
		const FilePath mavenPath = ApplicationSettings::getInstance()->getMavenPath();
		const FilePath projectRootPath = m_settings->getMavenProjectFilePathExpandedAndAbsolute().getParentDirectory();

		std::shared_ptr<DialogView> dialogView = Application::getInstance()->getDialogView(DialogView::UseCase::PROJECT_SETUP);
		dialogView->showUnknownProgressDialog(L"Preparing Project", L"Maven\nGenerating Source Files");

		ScopedFunctor dialogHider([&dialogView](){
			dialogView->hideUnknownProgressDialog();
		});

		const std::wstring errorMessage = utility::mavenGenerateSources(mavenPath, projectRootPath);
		if (!errorMessage.empty())
		{
			MessageStatus(errorMessage, true, false).dispatch();
			Application::getInstance()->handleDialog(errorMessage);
			return false;
		}

		dialogView->showUnknownProgressDialog(L"Preparing Project", L"Maven\nExporting Dependencies");

		bool success = utility::mavenCopyDependencies(
			mavenPath, projectRootPath, m_settings->getMavenDependenciesDirectoryPath()
		);

		return success;
	}

	return true;
}
