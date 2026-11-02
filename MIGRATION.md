# Source migration

Firmware engine and fwctl were extracted from linux-bsp/EDK at e1f2be34482610c3ea6e4b624fba765a6252cd2a. OTA server, CLI, agent and tests were extracted from linux-bsp/bsp-base-external at a8e78bb9de8265813a92aea6a4f4daf751295c96. Existing per-file license notices are retained; the project does not claim a blanket license for previously unlicensed files.

fwctl/adapter contains the MIT-licensed peripheral supervisor IPC adapter. Its wire ABI remains compatible with EDK system_manager; Device Manager does not link EDK libraries. Changes to this adapter require the cross-repository protocol tests.

The GPL-2.0 license text for files marked GPL-2.0+ is included in LICENSE.GPL-2.0. Other files retain their individual notices; this file does not change their licensing.
