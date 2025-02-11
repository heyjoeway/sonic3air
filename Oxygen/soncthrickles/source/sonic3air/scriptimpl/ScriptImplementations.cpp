/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2021 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/scriptimpl/ScriptImplementations.h"

#include "oxygen/application/video/VideoOut.h"
#include "oxygen/rendering/utils/Kosinski.h"
#include "oxygen/simulation/EmulatorInterface.h"

#include <lemon/program/FunctionWrapper.h>
#include <lemon/program/Module.h>


namespace s3air
{

	void kosinskiDecompress()
	{
		EmulatorInterface& emulatorInterface = EmulatorInterface::instance();
		uint32& A0 = emulatorInterface.getRegister(EmulatorInterface::Register::A0);
		uint32& A1 = emulatorInterface.getRegister(EmulatorInterface::Register::A1);

		uint8* initialPointer = emulatorInterface.getMemoryPointer(A1, false, 1);
		uint8* pointer = initialPointer;
		Kosinski::decompress(pointer, A0);

		A1 += (uint32)(pointer - initialPointer);
	}

	void writeScrollOffsetsShared(uint32 value)
	{
		const int height = VideoOut::instance().getScreenHeight();
		for (int line = 0; line < height; ++line)
		{
			EmulatorInterface::instance().writeMemory32(0xffffe000 + line * 4, value);
		}
	}

	void writeScrollOffsets()
	{
		const uint16 foregroundX = -(int16)EmulatorInterface::instance().readMemory16(0xffffee80);
		const uint16 backgroundX = -(int16)EmulatorInterface::instance().readMemory16(0xffffee8c);
		writeScrollOffsetsShared(((uint32)foregroundX << 16) | backgroundX);
	}

	void writeScrollOffsetsFlipped()
	{
		const uint16 foregroundX = -(int16)EmulatorInterface::instance().readMemory16(0xffffee80);
		const uint16 backgroundX = -(int16)EmulatorInterface::instance().readMemory16(0xffffee8c);
		writeScrollOffsetsShared(((uint32)backgroundX << 16) | foregroundX);
	}

	uint32 putNybbles(uint32 input, uint16 count, uint8 value)
	{
		for (uint16 i = 0; i < count; ++i)
		{
			input = (input << 4) | value;
		}
		return input;
	}


	// TEST!
	void decompressKosinskiData(uint32 sourceAddress, uint16 targetInVRAM)
	{
		EmulatorInterface& emulatorInterface = EmulatorInterface::instance();

		// Get the decompressed size
		uint16 size = emulatorInterface.readMemory16(sourceAddress);
		if (size == 0xa000)
			size = 0x8000;
		uint32 inputAddress = sourceAddress + 2;

		while (size > 0)
		{
			uint8 buffer[0x1000];
			uint8* pointer = buffer;
			Kosinski::decompress(pointer, inputAddress);

			const uint16 bytes = std::min<uint16>(size, 0x1000);
			uint8* dst = emulatorInterface.getVRam() + targetInVRAM;
			const uint8* src = buffer;
			for (uint16 i = 0; i < bytes; i += 2)
			{
				dst[0] = src[1];
				dst[1] = src[0];
				src += 2;
				dst += 2;
			}

			if (size < 0x1000)
				break;

			targetInVRAM += 0x1000;
			size -= bytes;
			inputAddress += 8;	// This is needed, but why...?
		}
	}

}



void ScriptImplementations::registerScriptBindings(lemon::Module& module)
{
	const uint8 defaultFlags = lemon::UserDefinedFunction::FLAG_ALLOW_INLINE_EXECUTION;
	module.addUserDefinedFunction("Kosinski.Decompress", lemon::wrap(&s3air::kosinskiDecompress), defaultFlags);
	module.addUserDefinedFunction("WriteScrollOffsets", lemon::wrap(&s3air::writeScrollOffsets), defaultFlags);
	module.addUserDefinedFunction("WriteScrollOffsetsFlipped", lemon::wrap(&s3air::writeScrollOffsetsFlipped), defaultFlags);
	module.addUserDefinedFunction("putNybbles", lemon::wrap(&s3air::putNybbles), defaultFlags);

	// TEST!
	module.addUserDefinedFunction("uncompressKosinskiData", lemon::wrap(&s3air::decompressKosinskiData), defaultFlags);
}
