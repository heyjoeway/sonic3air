#include "engineapp/pch.h"
#include "engineapp/EngineDelegate.h"
#include "engineapp/GameApp.h"
#include "engineapp/audio/AudioOut.h"


const EngineDelegateInterface::AppMetaData& EngineDelegate::getAppMetaData()
{
	if (mAppMetaData.mTitle.empty())
	{
		mAppMetaData.mTitle = "Oxygen Engine";
		mAppMetaData.mBuildVersion = "pre-alpha";
	}
	mAppMetaData.mAppDataFolder = L"OxygenEngine";
	return mAppMetaData;
}

GuiBase& EngineDelegate::createGameApp()
{
	return *new GameApp();
}

AudioOutBase& EngineDelegate::createAudioOut()
{
	return *new AudioOut();
}

bool EngineDelegate::onEnginePreStartup()
{
	return true;
}

bool EngineDelegate::setupCustomGameProfile()
{
	// Return false to signal that there's no custom game profile, and the oxygenproject.json should be loaded instead
	return false;
}

void EngineDelegate::startupGame()
{
}

void EngineDelegate::shutdownGame()
{
}

void EngineDelegate::updateGame(float timeElapsed)
{
}

void EngineDelegate::registerScriptBindings(lemon::Module& module)
{
#ifdef USE_EXPERIMENTS
	mExperiments.registerScriptBindings(module);
#endif
}

void EngineDelegate::registerNativizedCode(lemon::Program& program)
{
}

void EngineDelegate::onRuntimeInit(CodeExec& codeExec)
{
}

void EngineDelegate::onPreFrameUpdate()
{
#ifdef USE_EXPERIMENTS
	mExperiments.onPreFrameUpdate();
#endif
}

void EngineDelegate::onPostFrameUpdate()
{
#ifdef USE_EXPERIMENTS
	mExperiments.onPostFrameUpdate();
#endif
}

void EngineDelegate::onControlsUpdate()
{
}

void EngineDelegate::onPreSaveStateLoad()
{
}

bool EngineDelegate::mayLoadScriptMods()
{
	return true;
}

bool EngineDelegate::allowModdedData()
{
	return true;
}

bool EngineDelegate::useDeveloperFeatures()
{
	return true;
}

void EngineDelegate::onGameRecordingHeaderLoaded(const std::string& buildString, const std::vector<uint8>& buffer)
{
}

void EngineDelegate::onGameRecordingHeaderSave(std::vector<uint8>& buffer)
{
}

Font& EngineDelegate::getDebugFont(int size)
{
	if (size >= 10)
	{
		static Font font10;
		if (font10.getHeight() == 0)
			font10.load("data/font/freefont_pixeled.json", 0.0f);
		return font10;
	}
	else
	{
		static Font font3;
		if (font3.getHeight() == 0)
			font3.load("data/font/smallfont.json", 0.0f);
		return font3;
	}
}

void EngineDelegate::fillDebugVisualization(Bitmap& bitmap, int& mode)
{
}
