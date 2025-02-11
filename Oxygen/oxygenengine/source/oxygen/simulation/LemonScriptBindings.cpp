/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2021 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/simulation/LemonScriptBindings.h"
#include "oxygen/simulation/CodeExec.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/LogDisplay.h"
#include "oxygen/simulation/PersistentData.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/analyse/ROMDataAnalyser.h"
#include "oxygen/application/Application.h"
#include "oxygen/application/EngineMain.h"
#include "oxygen/application/audio/AudioOutBase.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/input/InputManager.h"
#include "oxygen/application/modding/ModManager.h"
#include "oxygen/application/overlays/DebugSidePanel.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/resources/ResourcesCache.h"
#include "oxygen/resources/SpriteCache.h"

#include <lemon/program/FunctionWrapper.h>
#include <lemon/program/Module.h>
#include <lemon/runtime/Runtime.h>
#include <lemon/runtime/StandardLibrary.h>

#include <rmxmedia.h>


namespace
{
	namespace detail
	{
		uint32 loadData(uint32 targetAddress, const std::vector<uint8>& data, uint32 offset, uint32 maxBytes)
		{
			if (data.empty())
				return 0;

			uint32 bytes = (uint32)data.size();
			if (offset != 0)
			{
				if (offset >= bytes)
					return 0;
				bytes -= offset;
			}
			if (maxBytes != 0)
			{
				bytes = std::min(bytes, maxBytes);
			}

			uint8* dst = EmulatorInterface::instance().getMemoryPointer(targetAddress, true, bytes);
			memcpy(dst, &data[offset], bytes);
			return bytes;
		}

		const std::string* tryResolveString(uint64 stringKey)
		{
			lemon::Runtime* runtime = lemon::Runtime::getActiveRuntime();
			RMX_ASSERT(nullptr != runtime, "No active lemon script runtime");

			const lemon::StoredString* str = runtime->resolveStringByKey(stringKey);
			RMX_CHECK(nullptr != str, "Could not resolve string from key", return nullptr);

			return &str->getString();
		}
	}


	DebugNotificationInterface* gDebugNotificationInterface = nullptr;

	void scriptAssert1(uint8 condition, uint64 text)
	{
		if (!condition)
		{
			std::string locationText = LemonScriptRuntime::getCurrentScriptLocationString();
			RMX_ASSERT(!locationText.empty(), "No active lemon script runtime");

			const std::string* textString = (text == 0) ? nullptr : detail::tryResolveString(text);
			if (nullptr != textString)
			{
				RMX_ERROR("Script assertion failed:\n'" << *textString << "'.\nIn " << locationText << ".", );
			}
			else
			{
				RMX_ERROR("Script assertion failed in " << locationText << ".", );
			}
		}
	}

	void scriptAssert2(uint8 condition)
	{
		scriptAssert1(condition, 0);
	}


	uint8 checkFlags_equal()
	{
		return EmulatorInterface::instance().getFlagZ();
	}

	uint8 checkFlags_negative()
	{
		return EmulatorInterface::instance().getFlagN();
	}

	void setZeroFlagByValue(uint32 value)
	{
		// In contrast to the emulator, we use the zero flag in its original form: it gets set when value is zero
		EmulatorInterface::instance().setFlagZ(value == 0);
	}

	template<typename T>
	void setNegativeFlagByValue(T value)
	{
		const int bits = sizeof(T) * 8;
		EmulatorInterface::instance().setFlagN((value >> (bits - 1)) != 0);
	}

	void push(uint32 value)
	{
		uint32& A7 = EmulatorInterface::instance().getRegister(15);
		A7 -= 4;
		EmulatorInterface::instance().writeMemory32(A7, value);
	}

	uint32 pop()
	{
		uint32& A7 = EmulatorInterface::instance().getRegister(15);
		const uint32 result = EmulatorInterface::instance().readMemory32(A7);
		A7 += 4;
		return result;
	}

	uint16 get_status_register()
	{
		// Dummy implementation, only exists for compatibility
		return 0;
	}

	void set_status_register(uint16 parameter)
	{
		// Dummy implementation, only exists for compatibility
	}


	void copyMemory(uint32 destAddress, uint32 sourceAddress, uint32 bytes)
	{
		uint8* destPointer = EmulatorInterface::instance().getMemoryPointer(destAddress, true, bytes);
		uint8* sourcePointer = EmulatorInterface::instance().getMemoryPointer(sourceAddress, false, bytes);
		memcpy(destPointer, sourcePointer, bytes);
	}

	void zeroMemory(uint32 startAddress, uint32 bytes)
	{
		uint8* pointer = EmulatorInterface::instance().getMemoryPointer(startAddress, true, bytes);
		memset(pointer, 0, bytes);
	}

	void fillMemory_u8(uint32 startAddress, uint32 bytes, uint8 value)
	{
		uint8* pointer = EmulatorInterface::instance().getMemoryPointer(startAddress, true, bytes);
		for (uint32 i = 0; i < bytes; ++i)
		{
			pointer[i] = value;
		}
	}

	void fillMemory_u16(uint32 startAddress, uint32 bytes, uint16 value)
	{
		RMX_CHECK((startAddress & 0x01) == 0, "Odd address not valid", return);
		RMX_CHECK((bytes & 0x01) == 0, "Odd number of bytes not valid", return);

		uint8* pointer = EmulatorInterface::instance().getMemoryPointer(startAddress, true, bytes);

		value = (value << 8) + (value >> 8);
		for (uint32 i = 0; i < bytes; i += 2)
		{
			*(uint16*)(&pointer[i]) = value;
		}
	}

	void fillMemory_u32(uint32 startAddress, uint32 bytes, uint32 value)
	{
		RMX_CHECK((startAddress & 0x01) == 0, "Odd address not valid", return);
		RMX_CHECK((bytes & 0x03) == 0, "Number of bytes must be divisible by 4", return);

		uint8* pointer = EmulatorInterface::instance().getMemoryPointer(startAddress, true, bytes);

		value = ((value & 0x000000ff) << 24)
			  + ((value & 0x0000ff00) << 8)
			  + ((value & 0x00ff0000) >> 8)
			  + ((value & 0xff000000) >> 24);

		for (uint32 i = 0; i < bytes; i += 4)
		{
			*(uint32*)(&pointer[i]) = value;
		}
	}


	uint32 System_loadPersistentData(uint32 targetAddress, uint64 key, uint32 maxBytes)
	{
		const std::vector<uint8>& data = PersistentData::instance().getData(key);
		return detail::loadData(targetAddress, data, 0, maxBytes);
	}

	void System_savePersistentData(uint32 sourceAddress, uint64 key, uint32 bytes)
	{
		const size_t size = (size_t)bytes;
		std::vector<uint8> data;
		data.resize(size);
		const uint8* src = EmulatorInterface::instance().getMemoryPointer(sourceAddress, false, bytes);
		memcpy(&data[0], src, size);

		const std::string* keyString = detail::tryResolveString(key);
		if (nullptr == keyString)
			return;

		PersistentData::instance().setData(*keyString, data);
	}

	uint32 SRAM_load(uint32 address, uint16 offset, uint16 bytes)
	{
		return (uint32)EmulatorInterface::instance().loadSRAM(address, (size_t)offset, (size_t)bytes);
	}

	void SRAM_save(uint32 address, uint16 offset, uint16 bytes)
	{
		EmulatorInterface::instance().saveSRAM(address, (size_t)offset, (size_t)bytes);
	}


	void System_setupCallFrame2(uint64 functionName, uint64 labelName)
	{
		const std::string* functionNameString = detail::tryResolveString(functionName);
		if (nullptr == functionNameString)
			return;

		const std::string* labelNameString = nullptr;
		if (labelName != 0)
		{
			labelNameString = detail::tryResolveString(labelName);
		}

		CodeExec* codeExec = CodeExec::getActiveInstance();
		RMX_CHECK(nullptr != codeExec, "No running CodeExec instance", return);
		codeExec->setupCallFrame(*functionNameString, (nullptr == labelNameString) ? "" : *labelNameString);
	}

	void System_setupCallFrame1(uint64 functionName)
	{
		System_setupCallFrame2(functionName, 0);
	}

	uint32 System_rand()
	{
		RMX_ASSERT(RAND_MAX >= 0x0800, "RAND_MAX not high enough on this platform, adjustments needed");
		return ((uint32)(rand() & 0x03ff) << 22) + ((uint32)(rand() & 0x07ff) << 11) + (uint32)(rand() & 0x07ff);
	}

	uint32 System_getPlatformFlags()
	{
		return EngineMain::instance().getPlatformFlags();
	}

	bool System_hasPlatformFlag(uint32 flag)
	{
		return (System_getPlatformFlags() & flag) != 0;
	}

	bool System_hasExternalRawData(uint64 key)
	{
		const std::vector<const ResourcesCache::RawData*>& rawDataVector = ResourcesCache::instance().getRawData(key);
		return !rawDataVector.empty();
	}

	uint32 System_loadExternalRawData1(uint64 key, uint32 targetAddress, uint32 offset, uint32 maxBytes, bool loadOriginalData, bool loadModdedData)
	{
		const std::vector<const ResourcesCache::RawData*>& rawDataVector = ResourcesCache::instance().getRawData(key);
		const ResourcesCache::RawData* rawData = nullptr;
		for (int i = (int)rawDataVector.size() - 1; i >= 0; --i)
		{
			const ResourcesCache::RawData* candidate = rawDataVector[i];
			const bool allow = (candidate->mIsModded) ? loadModdedData : loadOriginalData;
			if (allow)
			{
				rawData = candidate;
				break;
			}
		}

		if (nullptr == rawData)
			return 0;

		return detail::loadData(targetAddress, rawData->mContent, offset, maxBytes);
	}

	uint32 System_loadExternalRawData2(uint64 key, uint32 targetAddress)
	{
		return System_loadExternalRawData1(key, targetAddress, 0, 0, true, true);
	}

	bool System_hasExternalPaletteData(uint64 key, uint8 line)
	{
		const ResourcesCache::Palette* palette = ResourcesCache::instance().getPalette(key, line);
		return (nullptr != palette);
	}

	uint16 System_loadExternalPaletteData(uint64 key, uint8 line, uint32 targetAddress, uint8 maxColors)
	{
		const ResourcesCache::Palette* palette = ResourcesCache::instance().getPalette(key, line);
		if (nullptr == palette)
			return 0;

		const std::vector<Color>& colors = palette->mColors;
		const size_t numColors = std::min<size_t>(colors.size(), maxColors);
		uint32* targetPointer = (uint32*)EmulatorInterface::instance().getMemoryPointer(targetAddress, true, (uint32)numColors * 4);
		for (size_t i = 0; i < numColors; ++i)
		{
			targetPointer[i] = palette->mColors[i].getRGBA32();
		}
		return (uint16)numColors;
	}


	void debugLogInternal(const std::string& valueString)
	{
		uint32 lineNumber = 0;
		const bool success = LemonScriptRuntime::getCurrentScriptFunction(nullptr, nullptr, &lineNumber, nullptr);
		RMX_ASSERT(success, "No active lemon script runtime");

		LogDisplay::ScriptLogSingleEntry& scriptLogSingleEntry = LogDisplay::instance().updateScriptLogValue(*String(0, "%04d", lineNumber), valueString);
		if (gDebugNotificationInterface)
			gDebugNotificationInterface->onLog(scriptLogSingleEntry);

		Application::instance().getSimulation().stopSingleStepContinue();
	}

	void logSetter(int64 value, bool decimal)
	{
		const std::string valueString = decimal ? *String(0, "%d", value) : *String(0, "%08x", value);
		debugLogInternal(valueString);
	}

	void debugLog(uint64 stringHash)
	{
		const std::string* str = detail::tryResolveString(stringHash);
		if (nullptr != str)
		{
			debugLogInternal(*str);
		}
	}

	void debugLogColors(uint64 name, uint32 startAddress, uint8 numColors)
	{
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			const std::string* str = detail::tryResolveString(name);
			if (nullptr == str)
				return;

			CodeExec* codeExec = CodeExec::getActiveInstance();
			RMX_CHECK(nullptr != codeExec, "No running CodeExec instance", return);
			EmulatorInterface& emulatorInterface = codeExec->getEmulatorInterface();

			LogDisplay::ColorLogEntry entry;
			entry.mName = *str;
			entry.mColors.reserve(numColors);
			for (uint8 i = 0; i < numColors; ++i)
			{
				const uint16 packedColor = emulatorInterface.readMemory16(startAddress + i * 2);
				entry.mColors.push_back(PaletteManager::unpackColor(packedColor));
			}
			LogDisplay::instance().addColorLogEntry(entry);

			Application::instance().getSimulation().stopSingleStepContinue();
		}
	}


	uint16 Input_getController(uint8 controllerIndex)
	{
		return (controllerIndex < 2) ? ControlsIn::instance().getInputPad((size_t)controllerIndex) : 0;
	}

	uint16 Input_getControllerPrevious(uint8 controllerIndex)
	{
		return (controllerIndex < 2) ? ControlsIn::instance().getPrevInputPad((size_t)controllerIndex) : 0;
	}

	bool getButtonState(int index, bool previousValue = false)
	{
		ControlsIn& controlsIn = ControlsIn::instance();
		const int playerIndex = (index & 0x10) ? 1 : 0;
		const uint16 bitmask = previousValue ? controlsIn.getPrevInputPad(playerIndex) : controlsIn.getInputPad(playerIndex);
		return ((bitmask >> (index & 0x0f)) & 1) != 0;
	}

	uint8 Input_buttonDown(uint8 index)
	{
		// Button down right now
		return getButtonState((int)index) ? 1 : 0;
	}

	uint8 Input_buttonPressed(uint8 index)
	{
		// Button down now, but not in previous frame
		return (getButtonState((int)index) && !getButtonState(index, true)) ? 1 : 0;
	}

	void Input_setTouchInputMode(uint8 index)
	{
		return InputManager::instance().setTouchInputMode((InputManager::TouchInputMode)index);
	}

	void Input_setControllerLEDs(uint8 playerIndex, uint32 color)
	{
		InputManager::instance().setControllerLEDsForPlayer(playerIndex, Color::fromABGR32(color));
	}


	void yieldExecution()
	{
		CodeExec* codeExec = CodeExec::getActiveInstance();
		RMX_CHECK(nullptr != codeExec, "No running CodeExec instance", return);
		codeExec->yieldExecution();
	}

	uint16 getScreenWidth()
	{
		return (uint16)VideoOut::instance().getScreenWidth();
	}

	uint16 getScreenHeight()
	{
		return (uint16)VideoOut::instance().getScreenHeight();
	}

	uint16 getScreenExtend()
	{
		return (uint16)(VideoOut::instance().getScreenWidth() - 320) / 2;
	}


	enum class WriteTarget
	{
		VRAM,
		VSRAM,
		CRAM
	};
	WriteTarget mWriteTarget = WriteTarget::VRAM;
	uint16 mWriteAddress = 0;
	uint16 mWriteIncrement = 2;

	void VDP_setupVRAMWrite(uint16 vramAddress)
	{
		mWriteTarget = WriteTarget::VRAM;
		mWriteAddress = vramAddress;
	}

	void VDP_setupVSRAMWrite(uint16 vsramAddress)
	{
		mWriteTarget = WriteTarget::VSRAM;
		mWriteAddress = vsramAddress;
	}

	void VDP_setupCRAMWrite(uint16 cramAddress)
	{
		mWriteTarget = WriteTarget::CRAM;
		mWriteAddress = cramAddress;
	}

	void VDP_setWriteIncrement(uint16 increment)
	{
		mWriteIncrement = increment;
	}

	uint16 VDP_readData16()
	{
		uint16 result;
		switch (mWriteTarget)
		{
			case WriteTarget::VRAM:
			{
				result = *(uint16*)(EmulatorInterface::instance().getVRam() + mWriteAddress);
				break;
			}

			case WriteTarget::VSRAM:
			{
				const uint8 index = (mWriteAddress / 2) & 0x3f;
				result = EmulatorInterface::instance().getVSRam()[index];
				break;
			}

			default:
			{
				RMX_ERROR("Not supported", );
				return 0;
			}
		}
		mWriteAddress += mWriteIncrement;
		return result;
	}

	uint32 VDP_readData32()
	{
		const uint16 hi = VDP_readData16();
		const uint16 lo = VDP_readData16();
		return ((uint32)hi << 16) | lo;
	}

	void VDP_writeData16(uint16 value)
	{
		switch (mWriteTarget)
		{
			case WriteTarget::VRAM:
			{
				if (nullptr != gDebugNotificationInterface)
					gDebugNotificationInterface->onVRAMWrite(mWriteAddress, 2);

				uint16* dst = (uint16*)(EmulatorInterface::instance().getVRam() + mWriteAddress);
				*dst = value;
				break;
			}

			case WriteTarget::VSRAM:
			{
				const uint8 index = (mWriteAddress / 2) & 0x3f;
				EmulatorInterface::instance().getVSRam()[index] = value;
				break;
			}

			case WriteTarget::CRAM:
			{
				RenderParts::instance().getPaletteManager().writePaletteEntryPacked(0, mWriteAddress / 2, value);
				break;
			}
		}
		mWriteAddress += mWriteIncrement;
	}

	void VDP_writeData32(uint32 value)
	{
		VDP_writeData16((uint16)(value >> 16));
		VDP_writeData16((uint16)value);
	}

	void VDP_copyToVRAM(uint32 address, uint16 bytes)
	{
		RMX_CHECK((bytes & 1) == 0, "Number of bytes in VDP_copyToVRAM must be divisible by two, but is " << bytes, bytes &= 0xfffe);
		RMX_CHECK(uint32(mWriteAddress) + bytes <= 0x10000, "Invalid VRAM access from " << rmx::hexString(mWriteAddress, 8) << " to " << rmx::hexString(mWriteAddress+bytes-1, 8) << " in VDP_copyToVRAM", return);

		if (nullptr != gDebugNotificationInterface)
			gDebugNotificationInterface->onVRAMWrite(mWriteAddress, bytes);

		EmulatorInterface& emulatorInterface = EmulatorInterface::instance();
		if (mWriteIncrement == 2)
		{
			// Optimized version of the code below
			uint16* dst = (uint16*)(emulatorInterface.getVRam() + mWriteAddress);
			const uint16* src = (uint16*)(emulatorInterface.getMemoryPointer(address, false, bytes));
			const uint16* end = src + (bytes / 2);
			for (; src != end; ++src, ++dst)
			{
				*dst = swapBytes16(*src);
			}
			mWriteAddress += bytes;
		}
		else
		{
			for (uint16 i = 0; i < bytes; i += 2)
			{
				uint16* dst = (uint16*)(emulatorInterface.getVRam() + mWriteAddress);
				*dst = emulatorInterface.readMemory16(address);
				mWriteAddress += mWriteIncrement;
				address += 2;
			}
		}
	}

	void VDP_fillVRAMbyDMA(uint16 fillValue, uint16 vramAddress, uint16 bytes)
	{
		RMX_CHECK(uint32(vramAddress) + bytes <= 0x10000, "Invalid VRAM access from " << rmx::hexString(vramAddress, 8) << " to " << rmx::hexString(uint32(vramAddress)+bytes-1, 8) << " in VDP_fillVRAMbyDMA", return);

		if (nullptr != gDebugNotificationInterface)
			gDebugNotificationInterface->onVRAMWrite(vramAddress, bytes);

		uint16* dst = (uint16*)(EmulatorInterface::instance().getVRam() + vramAddress);
		for (uint16 i = 0; i < bytes; i += 2)
		{
			*dst = fillValue;
			++dst;
		}
		mWriteAddress = vramAddress + bytes;
	}

	void VDP_zeroVRAM(uint16 bytes)
	{
		if (nullptr != gDebugNotificationInterface)
			gDebugNotificationInterface->onVRAMWrite(mWriteAddress, bytes);

		VDP_fillVRAMbyDMA(0, mWriteAddress, bytes);
	}

	void VDP_copyToCRAM(uint32 address, uint16 bytes)
	{
		RMX_ASSERT(mWriteAddress < 0x80 && mWriteAddress + bytes <= 0x80, "Invalid write access to CRAM");
		RMX_ASSERT((mWriteAddress % 2) == 0, "Invalid CRAM write address " << mWriteAddress);
		RMX_ASSERT((mWriteIncrement % 2) == 0, "Invalid CRAM write increment " << mWriteIncrement);

		PaletteManager& paletteManager = RenderParts::instance().getPaletteManager();
		for (uint16 i = 0; i < bytes; i += 2)
		{
			const uint16 colorValue = EmulatorInterface::instance().readMemory16(address + i);
			paletteManager.writePaletteEntryPacked(0, mWriteAddress / 2, colorValue);
			mWriteAddress += mWriteIncrement;
		}
	}

	void VDP_copyToVRAMbyDMA(uint32 sourceAddress, uint16 vramAddress, uint16 bytes)
	{
		VDP_setupVRAMWrite(vramAddress);
		VDP_copyToVRAM(sourceAddress, bytes);
	}

	void VDP_copyToCRAMbyDMA(uint32 sourceAddress, uint16 vramAddress, uint16 bytes)
	{
		VDP_setupCRAMWrite(vramAddress);
		VDP_copyToCRAM(sourceAddress, bytes);
	}


	void VDP_Config_setActiveDisplay(uint8 enable)
	{
		RenderParts::instance().setActiveDisplay(enable != 0);
	}

	void VDP_Config_setNameTableBasePlaneB(uint16 vramAddress)
	{
		RenderParts::instance().getPlaneManager().setNameTableBaseB(vramAddress);
	}

	void VDP_Config_setNameTableBasePlaneA(uint16 vramAddress)
	{
		RenderParts::instance().getPlaneManager().setNameTableBaseA(vramAddress);
	}

	void VDP_Config_setNameTableBasePlaneW(uint16 vramAddress)
	{
		RenderParts::instance().getPlaneManager().setNameTableBaseW(vramAddress);
	}

	void VDP_Config_setVerticalScrolling(uint8 verticalScrolling, uint8 horizontalScrollMask)
	{
		RenderParts::instance().getScrollOffsetsManager().setVerticalScrolling(verticalScrolling != 0);
		RenderParts::instance().getScrollOffsetsManager().setHorizontalScrollMask(horizontalScrollMask);
	}

	void VDP_Config_setBackdropColor(uint8 paletteIndex)
	{
		RenderParts::instance().getPaletteManager().setBackdropColorIndex(paletteIndex);
	}

	void VDP_Config_setRenderingModeConfiguration(uint8 shadowHighlightPalette)
	{
		// TODO: Implement this
	}

	void VDP_Config_setHorizontalScrollTableBase(uint16 vramAddress)
	{
		RenderParts::instance().getScrollOffsetsManager().setHorizontalScrollTableBase(vramAddress);
	}

	void VDP_Config_setPlayfieldSizeInPatterns(uint16 width, uint16 height)
	{
		RenderParts::instance().getPlaneManager().setPlayfieldSizeInPatterns(Vec2i(width, height));
	}

	void VDP_Config_setPlayfieldSizeInPixels(uint16 width, uint16 height)
	{
		RenderParts::instance().getPlaneManager().setPlayfieldSizeInPixels(Vec2i(width, height));
	}

	void VDP_Config_setupWindowPlane(uint8 useWindowPlane, uint16 splitY)
	{
		RenderParts::instance().getPlaneManager().setupPlaneW(useWindowPlane != 0, splitY);
		RenderParts::instance().getScrollOffsetsManager().setPlaneWScrollOffset(Vec2i(0, 0));	// Reset scroll offset to default
	}

	void VDP_Config_setPlaneWScrollOffset(uint16 x, uint8 y)
	{
		RenderParts::instance().getScrollOffsetsManager().setPlaneWScrollOffset(Vec2i(x, y));
	}

	void VDP_Config_setSpriteAttributeTableBase(uint16 vramAddress)
	{
		RenderParts::instance().getSpriteManager().setSpriteAttributeTableBase(vramAddress);
	}


	uint16 getVRAM(uint16 vramAddress)
	{
		return *(uint16*)(&EmulatorInterface::instance().getVRam()[vramAddress]);
	}

	void setVRAM(uint16 vramAddress, uint16 value)
	{
		*(uint16*)(&EmulatorInterface::instance().getVRam()[vramAddress]) = value;
	}


	void Renderer_setPaletteEntry(uint8 index, uint32 color)
	{
		RenderParts::instance().getPaletteManager().writePaletteEntry(0, index, color);
	}

	void Renderer_setPaletteEntryPacked(uint8 index, uint16 color)
	{
		RenderParts::instance().getPaletteManager().writePaletteEntryPacked(0, index, color);
	}

	void Renderer_enableSecondaryPalette(uint8 line)
	{
		RenderParts::instance().getPaletteManager().setPaletteSplitPositionY(line);
	}

	void Renderer_setSecondaryPaletteEntryPacked(uint8 index, uint16 color)
	{
		RenderParts::instance().getPaletteManager().writePaletteEntryPacked(1, index, color);
	}

	void Renderer_setScrollOffsetH(uint8 setIndex, uint16 lineNumber, uint16 value)
	{
		RenderParts::instance().getScrollOffsetsManager().overwriteScrollOffsetH(setIndex, lineNumber, value);
	}

	void Renderer_setScrollOffsetV(uint8 setIndex, uint16 rowNumber, uint16 value)
	{
		RenderParts::instance().getScrollOffsetsManager().overwriteScrollOffsetV(setIndex, rowNumber, value);
	}

	void Renderer_setHorizontalScrollNoRepeat(uint8 setIndex, uint8 enable)
	{
		RenderParts::instance().getScrollOffsetsManager().setHorizontalScrollNoRepeat(setIndex, enable != 0);
	}

	void Renderer_setVerticalScrollOffsetBias(int16 bias)
	{
		RenderParts::instance().getScrollOffsetsManager().setVerticalScrollOffsetBias(bias);
	}

	void Renderer_enforceClearScreen(uint8 enabled)
	{
		RenderParts::instance().setEnforceClearScreen(enabled != 0);
	}

	void Renderer_enableDefaultPlane(uint8 planeIndex, uint8 enabled)
	{
		RenderParts::instance().getPlaneManager().setDefaultPlaneEnabled(planeIndex, enabled != 0);
	}

	void Renderer_setupPlane(int16 px, int16 py, int16 width, int16 height, uint8 planeIndex, uint8 scrollOffsets, uint16 renderQueue)
	{
		RenderParts::instance().getPlaneManager().setupCustomPlane(Recti(px, py, width, height), planeIndex, scrollOffsets, renderQueue);
	}

	void Renderer_resetCustomPlaneConfigurations()
	{
		RenderParts::instance().getPlaneManager().resetCustomPlanes();
	}

	void Renderer_resetSprites()
	{
		RenderParts::instance().getSpriteManager().resetSprites();
	}

	void Renderer_drawVdpSprite(int16 px, int16 py, uint8 encodedSize, uint16 patternIndex, uint16 renderQueue)
	{
		RenderParts::instance().getSpriteManager().drawVdpSprite(Vec2i(px, py), encodedSize, patternIndex, renderQueue);
	}

	void Renderer_drawVdpSpriteWithAlpha(int16 px, int16 py, uint8 encodedSize, uint16 patternIndex, uint16 renderQueue, uint8 alpha)
	{
		RenderParts::instance().getSpriteManager().drawVdpSprite(Vec2i(px, py), encodedSize, patternIndex, renderQueue, Color(1.0f, 1.0f, 1.0f, (float)alpha / 255.0f));
	}

	void Renderer_drawVdpSpriteWithTint(int16 px, int16 py, uint8 encodedSize, uint16 patternIndex, uint16 renderQueue, uint32 tintColor, uint32 addedColor)
	{
		RenderParts::instance().getSpriteManager().drawVdpSprite(Vec2i(px, py), encodedSize, patternIndex, renderQueue, Color::fromABGR32(tintColor), Color::fromABGR32(addedColor));
	}

	bool Renderer_hasCustomSprite(uint64 key)
	{
		return SpriteCache::instance().hasSprite(key);
	}

	uint64 Renderer_setupCustomUncompressedSprite(uint32 sourceBase, uint16 words, uint32 mappingOffset, uint8 animationSprite, uint8 atex)
	{
		return SpriteCache::instance().setupSpriteFromROM(sourceBase, words / 0x10, mappingOffset, animationSprite, atex, SpriteCache::ENCODING_NONE);
	}

	uint64 Renderer_setupCustomCharacterSprite(uint32 sourceBase, uint32 tableAddress, uint32 mappingOffset, uint8 animationSprite, uint8 atex)
	{
		return SpriteCache::instance().setupSpriteFromROM(sourceBase, tableAddress, mappingOffset, animationSprite, atex, SpriteCache::ENCODING_CHARACTER);
	}

	uint64 Renderer_setupCustomObjectSprite(uint32 sourceBase, uint32 tableAddress, uint32 mappingOffset, uint8 animationSprite, uint8 atex)
	{
		return SpriteCache::instance().setupSpriteFromROM(sourceBase, tableAddress, mappingOffset, animationSprite, atex, SpriteCache::ENCODING_OBJECT);
	}

	uint64 Renderer_setupKosinskiCompressedSprite1(uint32 sourceAddress, uint32 mappingOffset, uint8 animationSprite, uint8 atex)
	{
		return SpriteCache::instance().setupSpriteFromROM(sourceAddress, 0, mappingOffset, animationSprite, atex, SpriteCache::ENCODING_KOSINSKI);
	}

	uint64 Renderer_setupKosinskiCompressedSprite2(uint32 sourceAddress, uint32 mappingOffset, uint8 animationSprite, uint8 atex, int16 indexOffset)
	{
		return SpriteCache::instance().setupSpriteFromROM(sourceAddress, 0, mappingOffset, animationSprite, atex, SpriteCache::ENCODING_KOSINSKI, indexOffset);
	}

	void Renderer_drawCustomSprite1(uint64 key, int16 px, int16 py, uint8 atex, uint8 flags, uint16 renderQueue)
	{
		RenderParts::instance().getSpriteManager().drawCustomSprite(key, Vec2i(px, py), atex, flags, renderQueue);
	}

	void Renderer_drawCustomSprite2(uint64 key, int16 px, int16 py, uint8 atex, uint8 flags, uint16 renderQueue, uint8 angle, uint8 alpha)
	{
		RenderParts::instance().getSpriteManager().drawCustomSprite(key, Vec2i(px, py), atex, flags, renderQueue, Color(1.0f, 1.0f, 1.0f, (float)alpha / 255.0f), (float)angle / 128.0f * PI_FLOAT);
	}

	void Renderer_drawCustomSprite3(uint64 key, int16 px, int16 py, uint8 atex, uint8 flags, uint16 renderQueue, uint8 angle, uint32 tint, int32 scale)
	{
		RenderParts::instance().getSpriteManager().drawCustomSprite(key, Vec2i(px, py), atex, flags, renderQueue, Color::fromABGR32(tint), (float)angle / 128.0f * PI_FLOAT, (float)scale / 65536.0f);
	}

	void Renderer_drawCustomSpriteWithTransform(uint64 key, int16 px, int16 py, uint8 atex, uint8 flags, uint16 renderQueue, uint32 tint, int32 transform11, int32 transform12, int32 transform21, int32 transform22)
	{
		Transform2D transformation;
		transformation.setByMatrix((float)transform11 / 65536.0f, (float)transform12 / 65536.0f, (float)transform21 / 65536.0f, (float)transform22 / 65536.0f);
		RenderParts::instance().getSpriteManager().drawCustomSpriteWithTransform(key, Vec2i(px, py), atex, flags, renderQueue, Color::fromABGR32(tint), transformation);
	}

	void Renderer_extractCustomSprite(uint64 key, uint64 categoryName, uint8 spriteNumber, uint8 atex)
	{
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			const std::string* categoryNameString = detail::tryResolveString(categoryName);
			if (nullptr == categoryNameString)
				return;

			SpriteCache::instance().dumpSprite(key, *categoryNameString, spriteNumber, atex);
		}
	}

	void Renderer_addSpriteMask(int16 px, int16 py, int16 width, int16 height, uint16 renderQueue, uint8 priorityFlag)
	{
		RenderParts::instance().getSpriteManager().addSpriteMask(Vec2i(px, py), Vec2i(width, height), renderQueue, priorityFlag != 0, SpriteManager::Space::SCREEN);
	}

	void Renderer_addSpriteMaskWorld(int16 px, int16 py, int16 width, int16 height, uint16 renderQueue, uint8 priorityFlag)
	{
		RenderParts::instance().getSpriteManager().addSpriteMask(Vec2i(px, py), Vec2i(width, height), renderQueue, priorityFlag != 0, SpriteManager::Space::WORLD);
	}

	void Renderer_setLogicalSpriteSpace(uint8 space)
	{
		RMX_CHECK(space < 2, "Invalid space index " << space, return);
		RenderParts::instance().getSpriteManager().setLogicalSpriteSpace((SpriteManager::Space)space);
	}

	void Renderer_clearSpriteTag()
	{
		RenderParts::instance().getSpriteManager().clearSpriteTag();
	}

	void Renderer_setSpriteTagWithPosition(uint64 spriteTag, uint16 px, uint16 py)
	{
		RenderParts::instance().getSpriteManager().setSpriteTagWithPosition(spriteTag, Vec2i(px, py));
	}

	void Renderer_resetViewport(uint16 renderQueue)
	{
		RenderParts::instance().addViewport(Recti(0, 0, VideoOut::instance().getScreenWidth(), VideoOut::instance().getScreenHeight()), renderQueue);
	}

	void Renderer_setViewport(int16 px, int16 py, int16 width, int16 height, uint16 renderQueue)
	{
		RenderParts::instance().addViewport(Recti(px, py, width, height), renderQueue);
	}

	void Renderer_setGlobalComponentTint(int16 tintR, int16 tintG, int16 tintB, int16 addedR, int16 addedG, int16 addedB)
	{
		const Color tintColor((float)tintR / 255.0f, (float)tintG / 255.0f, (float)tintB / 255.0f, 1.0f);
		const Color addedColor((float)addedR / 255.0f, (float)addedG / 255.0f, (float)addedB / 255.0f, 0.0f);
		RenderParts::instance().getPaletteManager().setGlobalComponentTint(tintColor, addedColor);
	}


	bool Audio_isPlayingAudio(uint64 id)
	{
		return EngineMain::instance().getAudioOut().isPlayingSfxId(id);
	}

	void Audio_playAudio1(uint64 sfxId, uint8 contextId)
	{
		EngineMain::instance().getAudioOut().playAudioBase(sfxId, contextId);
	}

	void Audio_playAudio2(uint64 sfxId)
	{
		Audio_playAudio1(sfxId, 0x01);	// In-game sound effect context
	}

	void Audio_playOverride(uint64 sfxId, uint8 contextId, uint8 channelId, uint8 overriddenChannelId)
	{
		EngineMain::instance().getAudioOut().playOverride(sfxId, contextId, channelId, overriddenChannelId);
	}

	void Audio_stopChannel(uint8 channel)
	{
		EngineMain::instance().getAudioOut().stopChannel(channel);
	}

	void Audio_fadeOutChannel(uint8 channel, uint16 length)
	{
		EngineMain::instance().getAudioOut().fadeOutChannel(channel, (float)length / 256.0f);
	}

	void Audio_fadeInChannel(uint8 channel, uint16 length)
	{
		EngineMain::instance().getAudioOut().fadeInChannel(channel, (float)length / 256.0f);
	}

	void Audio_enableAudioModifier(uint8 channel, uint8 context, uint64 postfix, uint32 relativeSpeed)
	{
		const std::string* postfixString = detail::tryResolveString(postfix);
		if (nullptr == postfixString)
			return;

		EngineMain::instance().getAudioOut().enableAudioModifier(channel, context, *postfixString, (float)relativeSpeed / 65536.0f);
	}

	void Audio_disableAudioModifier(uint8 channel, uint8 context)
	{
		EngineMain::instance().getAudioOut().disableAudioModifier(channel, context);
	}


	const Mod* getActiveModByNameHash(uint64 modName)
	{
		const std::string* modNameString = detail::tryResolveString(modName);
		if (nullptr == modNameString)
			return nullptr;

		// TODO: This can be optimized with a lookup map by mod name hash (which we already have from the parameter)
		const auto& activeMods = ModManager::instance().getActiveMods();
		for (const Mod* mod : activeMods)
		{
			if (mod->mDisplayName == *modNameString)
				return mod;
		}
		return nullptr;
	}

	uint8 Mods_isModActive(uint64 modName)
	{
		const Mod* mod = getActiveModByNameHash(modName);
		return (nullptr != mod);
	}

	int32 Mods_getModPriority(uint64 modName)
	{
		const Mod* mod = getActiveModByNameHash(modName);
		return (nullptr != mod) ? (int32)mod->mActivePriority : -1;
	}


	void setWorldSpaceOffset(int32 px, int32 py)
	{
		// Note that this is needed for world space sprite masking, not only debug drawing
		RenderParts::instance().getSpriteManager().setWorldSpaceOffset(Vec2i(px, py));
	}

	void debugDrawRect(int32 px, int32 py, int32 sx, int32 sy)
	{
		RenderParts::instance().getOverlayManager().addDebugDrawRect(Recti(px, py, sx, sy));
	}

	void debugDrawRect2(int32 px, int32 py, int32 sx, int32 sy, uint32 color)
	{
		const Color rgba(((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f, ((color >> 24) & 0xff) / 255.0f);
		RenderParts::instance().getOverlayManager().addDebugDrawRect(Recti(px, py, sx, sy), rgba);
	}


	uint64 debugKeyGetter(int index)
	{
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			const int key = index + '0';
			return (FTX::keyState(key) && FTX::keyChange(key) && !FTX::keyState(SDLK_LALT) && !FTX::keyState(SDLK_RALT)) ? 1 : 0;
		}
		else
		{
			return 0;
		}
	}


	void debugWatch(uint32 address, uint16 bytes)
	{
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			CodeExec* codeExec = CodeExec::getActiveInstance();
			RMX_CHECK(nullptr != codeExec, "No running CodeExec instance", return);
			codeExec->addWatch(address, bytes, false);
		}
	}


	void debugDumpToFile(uint64 filename, uint32 startAddress, uint32 bytes)
	{
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			CodeExec* codeExec = CodeExec::getActiveInstance();
			RMX_CHECK(nullptr != codeExec, "No running CodeExec instance", return);
			EmulatorInterface& emulatorInterface = codeExec->getEmulatorInterface();
			const bool isValid = emulatorInterface.isValidMemoryRegion(startAddress, bytes);
			RMX_CHECK(isValid, "No valid memory region for debugDumpToFile: startAddress = " << rmx::hexString(startAddress, 6) << ", bytes = " << rmx::hexString(bytes, 2), return);

			const std::string* str = detail::tryResolveString(filename);
			if (nullptr == str)
				return;

			const uint8* src = emulatorInterface.getMemoryPointer(startAddress, false, bytes);
			FTX::FileSystem->saveFile(*str, src, (size_t)bytes);
		}
	}


	bool ROMDataAnalyser_isEnabled()
	{
		return Configuration::instance().mEnableROMDataAnalyzer;
	}

	bool ROMDataAnalyser_hasEntry(uint64 categoryHash, uint32 address)
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				const std::string* categoryName = detail::tryResolveString(categoryHash);
				if (nullptr != categoryName)
				{
					return analyser->hasEntry(*categoryName, address);
				}
			}
		}
		return false;
	}

	void ROMDataAnalyser_beginEntry(uint64 categoryHash, uint32 address)
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				const std::string* categoryName = detail::tryResolveString(categoryHash);
				if (nullptr != categoryName)
				{
					analyser->beginEntry(*categoryName, address);
				}
			}
		}
	}

	void ROMDataAnalyser_endEntry()
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				analyser->endEntry();
			}
		}
	}

	void ROMDataAnalyser_addKeyValue(uint64 keyHash, uint64 valueHash)
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				const std::string* keyString = detail::tryResolveString(keyHash);
				const std::string* valueString = detail::tryResolveString(valueHash);
				if (nullptr != keyString && nullptr != valueString)
				{
					analyser->addKeyValue(*keyString, *valueString);
				}
			}
		}
	}

	void ROMDataAnalyser_beginObject(uint64 keyHash)
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				const std::string* keyString = detail::tryResolveString(keyHash);
				if (nullptr != keyString)
				{
					analyser->beginObject(*keyString);
				}
			}
		}
	}

	void ROMDataAnalyser_endObject()
	{
		if (Configuration::instance().mEnableROMDataAnalyzer)
		{
			ROMDataAnalyser* analyser = Application::instance().getSimulation().getROMDataAnalyser();
			if (nullptr != analyser)
			{
				analyser->endObject();
			}
		}
	}

	bool System_SidePanel_setupCustomCategory(uint64 shortNameHash, uint64 fullNameHash)
	{
		const std::string* shortName = detail::tryResolveString(shortNameHash);
		const std::string* fullName = detail::tryResolveString(fullNameHash);
		if (nullptr == shortName || nullptr == fullName)
			return false;

		return Application::instance().getDebugSidePanel()->setupCustomCategory(*fullName, (*shortName)[0]);
	}

	bool System_SidePanel_addOption(uint64 stringHash, bool defaultValue)
	{
		const std::string* string = detail::tryResolveString(stringHash);
		if (nullptr == string)
			return false;

		return Application::instance().getDebugSidePanel()->addOption(*string, defaultValue);
	}

	void System_SidePanel_addEntry(uint64 key)
	{
		return Application::instance().getDebugSidePanel()->addEntry(key);
	}

	void System_SidePanel_addLine1(uint64 stringHash, int8 indent, uint32 color)
	{
		const std::string* string = detail::tryResolveString(stringHash);
		if (nullptr != string)
		{
			Application::instance().getDebugSidePanel()->addLine(*string, (int)indent, Color::fromABGR32(color));
		}
	}

	void System_SidePanel_addLine2(uint64 stringHash, int8 indent)
	{
		System_SidePanel_addLine1(stringHash, indent, 0xffffffff);
	}

	bool System_SidePanel_isEntryHovered(uint64 key)
	{
		return Application::instance().getDebugSidePanel()->isEntryHovered(key);
	}

	void System_writeDisplayLine(uint64 stringHash)
	{
		const std::string* str = detail::tryResolveString(stringHash);
		if (nullptr != str)
		{
			LogDisplay::instance().setLogDisplay(*str, 2.0f);
		}
	}

}


void LemonScriptBindings::registerBindings(lemon::Module& module)
{
	// Standard library
	lemon::StandardLibrary::registerBindings(module);

	const uint8 defaultFlags = lemon::UserDefinedFunction::FLAG_ALLOW_INLINE_EXECUTION;
	module.addUserDefinedFunction("assert", lemon::wrap(&scriptAssert1), defaultFlags);
	module.addUserDefinedFunction("assert", lemon::wrap(&scriptAssert2), defaultFlags);

	// Emulator interface bindings
	{
		EmulatorInterface& emulatorInterface = EmulatorInterface::instance();

		// Register access
		const std::string registerNamesDAR[16] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7" };
		for (size_t i = 0; i < 16; ++i)
		{
			lemon::ExternalVariable& var = module.addExternalVariable(registerNamesDAR[i], &lemon::PredefinedDataTypes::UINT_32);
			var.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_u8 = module.addExternalVariable(registerNamesDAR[i] + ".u8", &lemon::PredefinedDataTypes::UINT_8);
			var_u8.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_s8 = module.addExternalVariable(registerNamesDAR[i] + ".s8", &lemon::PredefinedDataTypes::INT_8);
			var_s8.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_u16 = module.addExternalVariable(registerNamesDAR[i] + ".u16", &lemon::PredefinedDataTypes::UINT_16);
			var_u16.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_s16 = module.addExternalVariable(registerNamesDAR[i] + ".s16", &lemon::PredefinedDataTypes::INT_16);
			var_s16.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_u32 = module.addExternalVariable(registerNamesDAR[i] + ".u32", &lemon::PredefinedDataTypes::UINT_32);
			var_u32.mPointer = &emulatorInterface.getRegister(i);

			lemon::ExternalVariable& var_s32 = module.addExternalVariable(registerNamesDAR[i] + ".s32", &lemon::PredefinedDataTypes::INT_32);
			var_s32.mPointer = &emulatorInterface.getRegister(i);
		}

		// Query flags
		module.addUserDefinedFunction("_equal", lemon::wrap(&checkFlags_equal), defaultFlags);
		module.addUserDefinedFunction("_negative", lemon::wrap(&checkFlags_negative), defaultFlags);

		// Explictly set flags
		module.addUserDefinedFunction("_setZeroFlagByValue", lemon::wrap(&setZeroFlagByValue), defaultFlags);
		module.addUserDefinedFunction("_setNegativeFlagByValue", lemon::wrap(&setNegativeFlagByValue<int8>), defaultFlags);
		module.addUserDefinedFunction("_setNegativeFlagByValue", lemon::wrap(&setNegativeFlagByValue<int16>), defaultFlags);
		module.addUserDefinedFunction("_setNegativeFlagByValue", lemon::wrap(&setNegativeFlagByValue<int32>), defaultFlags);

		// Memory access
		module.addUserDefinedFunction("copyMemory", lemon::wrap(&copyMemory), defaultFlags);
		module.addUserDefinedFunction("zeroMemory", lemon::wrap(&zeroMemory), defaultFlags);
		module.addUserDefinedFunction("fillMemory_u8", lemon::wrap(&fillMemory_u8), defaultFlags);
		module.addUserDefinedFunction("fillMemory_u16", lemon::wrap(&fillMemory_u16), defaultFlags);
		module.addUserDefinedFunction("fillMemory_u32", lemon::wrap(&fillMemory_u32), defaultFlags);

		// Push and pop
		module.addUserDefinedFunction("push", lemon::wrap(&push), defaultFlags);
		module.addUserDefinedFunction("pop", lemon::wrap(&pop), defaultFlags);

		// Status registers (for compatibility only)
		module.addUserDefinedFunction("get_status_register", lemon::wrap(&get_status_register), defaultFlags);
		module.addUserDefinedFunction("set_status_register", lemon::wrap(&set_status_register), defaultFlags);

		// Persistent data
		module.addUserDefinedFunction("System.loadPersistentData", lemon::wrap(&System_loadPersistentData), defaultFlags);
		module.addUserDefinedFunction("System.savePersistentData", lemon::wrap(&System_savePersistentData), defaultFlags);

		// SRAM
		module.addUserDefinedFunction("SRAM.load", lemon::wrap(&SRAM_load), defaultFlags);
		module.addUserDefinedFunction("SRAM.save", lemon::wrap(&SRAM_save), defaultFlags);

		// System
		module.addUserDefinedFunction("System.setupCallFrame", lemon::wrap(&System_setupCallFrame1));	// Should not get inline executed
		module.addUserDefinedFunction("System.setupCallFrame", lemon::wrap(&System_setupCallFrame2));	// Should not get inline executed
		module.addUserDefinedFunction("System.rand", lemon::wrap(&System_rand), defaultFlags);
		module.addUserDefinedFunction("System.getPlatformFlags", lemon::wrap(&System_getPlatformFlags), defaultFlags);
		module.addUserDefinedFunction("System.hasPlatformFlag", lemon::wrap(&System_hasPlatformFlag), defaultFlags);

		// Access external data
		module.addUserDefinedFunction("System.hasExternalRawData", lemon::wrap(&System_hasExternalRawData), defaultFlags);
		module.addUserDefinedFunction("System.loadExternalRawData", lemon::wrap(&System_loadExternalRawData1), defaultFlags);
		module.addUserDefinedFunction("System.loadExternalRawData", lemon::wrap(&System_loadExternalRawData2), defaultFlags);
		module.addUserDefinedFunction("System.hasExternalPaletteData", lemon::wrap(&System_hasExternalPaletteData), defaultFlags);
		module.addUserDefinedFunction("System.loadExternalPaletteData", lemon::wrap(&System_loadExternalPaletteData), defaultFlags);
	}

	// High-level functionality
	{
		// Input
		module.addUserDefinedFunction("Input.getController", lemon::wrap(&Input_getController), defaultFlags);
		module.addUserDefinedFunction("Input.getControllerPrevious", lemon::wrap(&Input_getControllerPrevious), defaultFlags);
		module.addUserDefinedFunction("buttonDown", lemon::wrap(&Input_buttonDown), defaultFlags);			// Deprecated
		module.addUserDefinedFunction("buttonPressed", lemon::wrap(&Input_buttonPressed), defaultFlags);	// Deprecated
		module.addUserDefinedFunction("Input.buttonDown", lemon::wrap(&Input_buttonDown), defaultFlags);
		module.addUserDefinedFunction("Input.buttonPressed", lemon::wrap(&Input_buttonPressed), defaultFlags);
		module.addUserDefinedFunction("Input.setTouchInputMode", lemon::wrap(&Input_setTouchInputMode), defaultFlags);
		module.addUserDefinedFunction("Input.setControllerLEDs", lemon::wrap(&Input_setControllerLEDs), defaultFlags);

		// Yield
		module.addUserDefinedFunction("yieldExecution", lemon::wrap(&yieldExecution));	// Should not get inline executed

		// Screen size query
		module.addUserDefinedFunction("getScreenWidth", lemon::wrap(&getScreenWidth), defaultFlags);
		module.addUserDefinedFunction("getScreenHeight", lemon::wrap(&getScreenHeight), defaultFlags);
		module.addUserDefinedFunction("getScreenExtend", lemon::wrap(&getScreenExtend), defaultFlags);

		// VDP emulation
		module.addUserDefinedFunction("VDP.setupVRAMWrite", lemon::wrap(&VDP_setupVRAMWrite), defaultFlags);
		module.addUserDefinedFunction("VDP.setupVSRAMWrite", lemon::wrap(&VDP_setupVSRAMWrite), defaultFlags);
		module.addUserDefinedFunction("VDP.setupCRAMWrite", lemon::wrap(&VDP_setupCRAMWrite), defaultFlags);
		module.addUserDefinedFunction("VDP.setWriteIncrement", lemon::wrap(&VDP_setWriteIncrement), defaultFlags);
		module.addUserDefinedFunction("VDP.readData16", lemon::wrap(&VDP_readData16), defaultFlags);
		module.addUserDefinedFunction("VDP.readData32", lemon::wrap(&VDP_readData32), defaultFlags);
		module.addUserDefinedFunction("VDP.writeData16", lemon::wrap(&VDP_writeData16), defaultFlags);
		module.addUserDefinedFunction("VDP.writeData32", lemon::wrap(&VDP_writeData32), defaultFlags);
		module.addUserDefinedFunction("VDP.copyToVRAM", lemon::wrap(&VDP_copyToVRAM), defaultFlags);
		module.addUserDefinedFunction("VDP.zeroVRAM", lemon::wrap(&VDP_zeroVRAM), defaultFlags);
		module.addUserDefinedFunction("VDP.copyToVRAMbyDMA", lemon::wrap(&VDP_copyToVRAMbyDMA), defaultFlags);
		module.addUserDefinedFunction("VDP.copyToCRAMbyDMA", lemon::wrap(&VDP_copyToCRAMbyDMA), defaultFlags);
		module.addUserDefinedFunction("VDP.fillVRAMbyDMA", lemon::wrap(&VDP_fillVRAMbyDMA), defaultFlags);

		// VDP config
		module.addUserDefinedFunction("VDP.Config.setActiveDisplay", lemon::wrap(&VDP_Config_setActiveDisplay), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setNameTableBasePlaneB", lemon::wrap(&VDP_Config_setNameTableBasePlaneB), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setNameTableBasePlaneA", lemon::wrap(&VDP_Config_setNameTableBasePlaneA), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setNameTableBasePlaneW", lemon::wrap(&VDP_Config_setNameTableBasePlaneW), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setBackdropColor", lemon::wrap(&VDP_Config_setBackdropColor), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setVerticalScrolling", lemon::wrap(&VDP_Config_setVerticalScrolling), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setRenderingModeConfiguration", lemon::wrap(&VDP_Config_setRenderingModeConfiguration), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setHorizontalScrollTableBase", lemon::wrap(&VDP_Config_setHorizontalScrollTableBase), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setPlayfieldSizeInPatterns", lemon::wrap(&VDP_Config_setPlayfieldSizeInPatterns), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setPlayfieldSizeInPixels", lemon::wrap(&VDP_Config_setPlayfieldSizeInPixels), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setupWindowPlane", lemon::wrap(&VDP_Config_setupWindowPlane), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setPlaneWScrollOffset", lemon::wrap(&VDP_Config_setPlaneWScrollOffset), defaultFlags);
		module.addUserDefinedFunction("VDP.Config.setSpriteAttributeTableBase", lemon::wrap(&VDP_Config_setSpriteAttributeTableBase), defaultFlags);

		// Direct VRAM access
		module.addUserDefinedFunction("getVRAM", lemon::wrap(&getVRAM), defaultFlags);
		module.addUserDefinedFunction("setVRAM", lemon::wrap(&setVRAM), defaultFlags);

		// Special renderer functionality
		module.addUserDefinedFunction("Renderer.setPaletteEntry", lemon::wrap(&Renderer_setPaletteEntry), defaultFlags);
		module.addUserDefinedFunction("Renderer.setPaletteEntryPacked", lemon::wrap(&Renderer_setPaletteEntryPacked), defaultFlags);
		module.addUserDefinedFunction("Renderer.enableSecondaryPalette", lemon::wrap(&Renderer_enableSecondaryPalette), defaultFlags);
		module.addUserDefinedFunction("Renderer.setSecondaryPaletteEntryPacked", lemon::wrap(&Renderer_setSecondaryPaletteEntryPacked), defaultFlags);
		module.addUserDefinedFunction("Renderer.setScrollOffsetH", lemon::wrap(&Renderer_setScrollOffsetH), defaultFlags);
		module.addUserDefinedFunction("Renderer.setScrollOffsetV", lemon::wrap(&Renderer_setScrollOffsetV), defaultFlags);
		module.addUserDefinedFunction("Renderer.setHorizontalScrollNoRepeat", lemon::wrap(&Renderer_setHorizontalScrollNoRepeat), defaultFlags);
		module.addUserDefinedFunction("Renderer.setVerticalScrollOffsetBias", lemon::wrap(&Renderer_setVerticalScrollOffsetBias), defaultFlags);
		module.addUserDefinedFunction("Renderer.enforceClearScreen", lemon::wrap(&Renderer_enforceClearScreen), defaultFlags);
		module.addUserDefinedFunction("Renderer.enableDefaultPlane", lemon::wrap(&Renderer_enableDefaultPlane), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupPlane", lemon::wrap(&Renderer_setupPlane), defaultFlags);
		module.addUserDefinedFunction("Renderer.resetCustomPlaneConfigurations", lemon::wrap(&Renderer_resetCustomPlaneConfigurations), defaultFlags);
		module.addUserDefinedFunction("Renderer.resetSprites", lemon::wrap(&Renderer_resetSprites), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawVdpSprite", lemon::wrap(&Renderer_drawVdpSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawVdpSpriteWithAlpha", lemon::wrap(&Renderer_drawVdpSpriteWithAlpha), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawVdpSpriteWithTint", lemon::wrap(&Renderer_drawVdpSpriteWithTint), defaultFlags);
		module.addUserDefinedFunction("Renderer.hasCustomSprite", lemon::wrap(&Renderer_hasCustomSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupCustomUncompressedSprite", lemon::wrap(&Renderer_setupCustomUncompressedSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupCustomCharacterSprite", lemon::wrap(&Renderer_setupCustomCharacterSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupCustomObjectSprite", lemon::wrap(&Renderer_setupCustomObjectSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupKosinskiCompressedSprite", lemon::wrap(&Renderer_setupKosinskiCompressedSprite1), defaultFlags);
		module.addUserDefinedFunction("Renderer.setupKosinskiCompressedSprite", lemon::wrap(&Renderer_setupKosinskiCompressedSprite2), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawCustomSprite", lemon::wrap(&Renderer_drawCustomSprite1), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawCustomSprite", lemon::wrap(&Renderer_drawCustomSprite2), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawCustomSprite", lemon::wrap(&Renderer_drawCustomSprite3), defaultFlags);
		module.addUserDefinedFunction("Renderer.drawCustomSpriteWithTransform", lemon::wrap(&Renderer_drawCustomSpriteWithTransform), defaultFlags);
		module.addUserDefinedFunction("Renderer.extractCustomSprite", lemon::wrap(&Renderer_extractCustomSprite), defaultFlags);
		module.addUserDefinedFunction("Renderer.addSpriteMask", lemon::wrap(&Renderer_addSpriteMask), defaultFlags);
		module.addUserDefinedFunction("Renderer.addSpriteMaskWorld", lemon::wrap(&Renderer_addSpriteMaskWorld), defaultFlags);
		module.addUserDefinedFunction("Renderer.setLogicalSpriteSpace", lemon::wrap(&Renderer_setLogicalSpriteSpace), defaultFlags);
		module.addUserDefinedFunction("Renderer.clearSpriteTag", lemon::wrap(&Renderer_clearSpriteTag), defaultFlags);
		module.addUserDefinedFunction("Renderer.setSpriteTagWithPosition", lemon::wrap(&Renderer_setSpriteTagWithPosition), defaultFlags);
		module.addUserDefinedFunction("Renderer.resetViewport", lemon::wrap(&Renderer_resetViewport), defaultFlags);
		module.addUserDefinedFunction("Renderer.setViewport", lemon::wrap(&Renderer_setViewport), defaultFlags);
		module.addUserDefinedFunction("Renderer.setGlobalComponentTint", lemon::wrap(&Renderer_setGlobalComponentTint), defaultFlags);

		// Audio
		module.addUserDefinedFunction("Audio.isPlayingAudio", lemon::wrap(&Audio_isPlayingAudio), defaultFlags);
		module.addUserDefinedFunction("Audio.playAudio", lemon::wrap(&Audio_playAudio1), defaultFlags);
		module.addUserDefinedFunction("Audio.playAudio", lemon::wrap(&Audio_playAudio2), defaultFlags);
		module.addUserDefinedFunction("Audio.stopChannel", lemon::wrap(&Audio_stopChannel), defaultFlags);
		module.addUserDefinedFunction("Audio.fadeInChannel", lemon::wrap(&Audio_fadeInChannel), defaultFlags);
		module.addUserDefinedFunction("Audio.fadeOutChannel", lemon::wrap(&Audio_fadeOutChannel), defaultFlags);
		module.addUserDefinedFunction("Audio.playOverride", lemon::wrap(&Audio_playOverride), defaultFlags);
		module.addUserDefinedFunction("Audio.enableAudioModifier", lemon::wrap(&Audio_enableAudioModifier), defaultFlags);
		module.addUserDefinedFunction("Audio.disableAudioModifier", lemon::wrap(&Audio_disableAudioModifier), defaultFlags);

		// Misc
		module.addUserDefinedFunction("Mods.isModActive", lemon::wrap(&Mods_isModActive), defaultFlags);
		module.addUserDefinedFunction("Mods.getModPriority", lemon::wrap(&Mods_getModPriority), defaultFlags);
	}

	// Debug features
	{
		// Debug log output
		{
			lemon::UserDefinedVariable& var = module.addUserDefinedVariable("Log", &lemon::PredefinedDataTypes::UINT_32);
			var.mSetter = std::bind(logSetter, std::placeholders::_1, false);
		}
		{
			lemon::UserDefinedVariable& var = module.addUserDefinedVariable("LogDec", &lemon::PredefinedDataTypes::UINT_32);
			var.mSetter = std::bind(logSetter, std::placeholders::_1, true);
		}

		module.addUserDefinedFunction("debugLog", lemon::wrap(&debugLog), defaultFlags);
		module.addUserDefinedFunction("debugLogColors", lemon::wrap(&debugLogColors), defaultFlags);

		// Debug draw
		{
			module.addUserDefinedFunction("setWorldSpaceOffset", lemon::wrap(&setWorldSpaceOffset), defaultFlags);
			module.addUserDefinedFunction("debugDrawRect", lemon::wrap(&debugDrawRect), defaultFlags);
			module.addUserDefinedFunction("debugDrawRect", lemon::wrap(&debugDrawRect2), defaultFlags);
		}

		// Debug keys
		for (int i = 0; i < 10; ++i)
		{
			lemon::UserDefinedVariable& var = module.addUserDefinedVariable("Key" + std::to_string(i), &lemon::PredefinedDataTypes::UINT_8);
			var.mGetter = std::bind(debugKeyGetter, i);
		}

		// Watches
		module.addUserDefinedFunction("debugWatch", lemon::wrap(&debugWatch), defaultFlags);

		// Dump to file
		module.addUserDefinedFunction("debugDumpToFile", lemon::wrap(&debugDumpToFile), defaultFlags);

		// ROM data analyser
		module.addUserDefinedFunction("ROMDataAnalyser.isEnabled",   lemon::wrap(&ROMDataAnalyser_isEnabled), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.hasEntry",    lemon::wrap(&ROMDataAnalyser_hasEntry), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.beginEntry",  lemon::wrap(&ROMDataAnalyser_beginEntry), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.endEntry",    lemon::wrap(&ROMDataAnalyser_endEntry), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.addKeyValue", lemon::wrap(&ROMDataAnalyser_addKeyValue), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.beginObject", lemon::wrap(&ROMDataAnalyser_beginObject), defaultFlags);
		module.addUserDefinedFunction("ROMDataAnalyser.endObject",   lemon::wrap(&ROMDataAnalyser_endObject), defaultFlags);

		// Debug side panel
		module.addUserDefinedFunction("System.SidePanel.setupCustomCategory", lemon::wrap(&System_SidePanel_setupCustomCategory), defaultFlags);
		module.addUserDefinedFunction("System.SidePanel.addOption", lemon::wrap(&System_SidePanel_addOption), defaultFlags);
		module.addUserDefinedFunction("System.SidePanel.addEntry", lemon::wrap(&System_SidePanel_addEntry), defaultFlags);
		module.addUserDefinedFunction("System.SidePanel.addLine", lemon::wrap(&System_SidePanel_addLine1), defaultFlags);
		module.addUserDefinedFunction("System.SidePanel.addLine", lemon::wrap(&System_SidePanel_addLine2), defaultFlags);
		module.addUserDefinedFunction("System.SidePanel.isEntryHovered", lemon::wrap(&System_SidePanel_isEntryHovered), defaultFlags);

		// This is not really debugging-related, as it's meant to be written in non-developer environment as well
		module.addUserDefinedFunction("System.writeDisplayLine", lemon::wrap(&System_writeDisplayLine), defaultFlags);
	}

	// Register game-specific script bindings
	EngineMain::getDelegate().registerScriptBindings(module);
}

void LemonScriptBindings::setDebugNotificationInterface(DebugNotificationInterface* debugNotificationInterface)
{
	gDebugNotificationInterface = debugNotificationInterface;
}
