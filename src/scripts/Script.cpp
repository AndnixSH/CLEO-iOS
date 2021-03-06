//
// Created by squ1dd13 on 08/11/2020.
//

#include "Script.h"

#include "bridge/Addresses.h"
#include "Mobile.h"
#include "Logging.h"

#include <sys/stat.h>

namespace Scripts {

    Script::Script(const std::string &path) {
        // Get the size so we know how much space to allocate.
        // CLEO scripts have some junk at the end (something to do with globals)
        //  that can't be executed, so the file size is not an accurate measure
        //  of the space taken up by instructions. There's enough RAM for that
        //  not to be a problem though.
        struct stat st {};
        stat(path.c_str(), &st);

        auto size = st.st_size;

        Log("Loading %s", path.c_str());

        std::FILE *scriptFile = std::fopen(path.c_str(), "rb");

        if (!scriptFile) {
            LogError("Failed to load script %s (unable to open file)", path.c_str());
        }

        // If we don't set activationTime to 0, it will get some junk value that may delay the script's launch.
        activationTime = 0;

        auto *data = new uint8[size];
        std::fread(data, 1, size, scriptFile);

        startPointer = currentPointer = data;

        std::fclose(scriptFile);

        static unsigned loadNumber = 0;

        // This is only the name until the script renames itself.
        std::string tempName = "magic" + std::to_string(loadNumber++);
        std::strcpy(name, tempName.c_str());
    }

    void Script::RunNextBlock() {
        // A 'block' ends when RunNextInstruction() returns a non-zero value.
        while (!RunNextInstruction()) {
            // Nothing
        }
    }

    uint8 Script::RunNextInstruction() {
        // This shouldn't happen, but just in case...
        if (!active) {
            return 1;
        }

        uint16 opcodeMask = *(uint16 *)currentPointer;
        currentPointer += 2;

        // A negative opcode is written when the return value is to be inverted.
        // The actual opcode and therefore operation to perform does not change.
        uint16 opcode = opcodeMask & 0x7fffu;
        invertReturn = opcodeMask & 0x8000u;

        // Check for a custom implementation (for mobile-specific instructions like touch checks).
        Mobile::Handler customHandler = Mobile::GetHandler(opcode);

        if (customHandler) {
            customHandler(this);
            return 0;
        }

        // TODO: Do we need to redirect startNewScript (0x4f)?
        if (opcode == 0x4e) {
            // 0x4e terminates the script, but we can't let the game terminate our scripts
            //  because it assumes they are part of its own system (which they aren't).
            // If we set the script as inactive, the destructor will be automatically called
            //  next time Scripts::Manager::AdvanceScripts runs.
            active = false;
            return 1;
        }

        // The game does some weird magic to work out what script pointer to pass when
        //  the opcode is in range of one of the calculated handlers.
        Script *scriptToPass = this;

        auto handler = FindHandler(opcode, scriptToPass);
        return handler(scriptToPass, opcode);
    }

    void Script::ReadValueArgs(uint32 count) {
        Memory::Call(Memory::Addresses::scriptReadNextArgs, this, count);
    }

    void *Script::ReadVariableArg() {
        return Memory::Call<void *>(Memory::Addresses::scriptReadVariable, this);
    }

    void Script::UpdateBoolean(int flag) {
        Memory::Call(Memory::Addresses::scriptFlagHandler, this, flag);
    }

    void Script::Unload() {
        delete[] startPointer;
        startPointer = nullptr;
    }

    Script::~Script() {
        Unload();
    }

    Script *Script::GetAlternateThis(uint64 handlerOffset) {
        auto handlerTable = Memory::Slid<uint64 *>(Memory::Addresses::opcodeHandlerTable);

        // TODO: Figure this one out.
        // The game WILL crash (inconsistently) if this value is not passed instead of 'this'.
        // This calculation is just from decompiler output.
        return (Script *)((long long)&this->nextScript +
                          (*(long long *)((long long)handlerTable + handlerOffset + 8) >> 1));
    }

    Script::OpcodeHandler Script::FindHandler(uint16 opcode, Script *&thisPtr) {
        static auto defaultHandler = Memory::Slid<OpcodeHandler>(Memory::Addresses::defaultOpcodeHandler);

        // Opcodes below 0xa8c are handled by functions from a table, and the rest are handled by defaultHandler.
        // The instructions are essentially handled by a giant 'switch' statement, and anything >= a8c goes to
        //  the default case.
        if (opcode >= 0xa8c) {
            return defaultHandler;
        }

        static auto handlerTable = Memory::Slid<OpcodeHandler *>(Memory::Addresses::opcodeHandlerTable);

        // https://repl.it/repls/PeriodicGlitteringSampler#main.py
        // This calculation just steps the address offset based on the opcode.
        // I would write it in a nicer way, but it works. (It's copied from Ghidra's decompilation.)
        uint64 handlerOffset = (uint64((opcode & 0x7fffu) * 1374389535llu) >> 33) & 0x3ffffff0;

        thisPtr = thisPtr->GetAlternateThis(handlerOffset);
        return handlerTable[handlerOffset / 8];
    }

    Script::Script(Script &&script) {
        // We can copy all the fields over using memcpy because they're all simple values.
        std::memcpy(this, &script, sizeof(Script));

        // Invalidate everything (including the buffer pointer, which is what we care most about).
        std::fill_n((uint8 *)&script, sizeof(Script), 0);
    }
}