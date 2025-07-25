#include "mos6502.h"
#include <iostream>
#include <cstring>
#include "util.h"

#define DELAY_CYCLES(map, opcode)              \
	for (int i = 0; i < map[opcode].cycles; ++i) \
	delayMicros(CLOCK_uS)
#define IS_BIT_ON(n, i) ((n & (1 << i)) == (1 << i))

using namespace std::placeholders;

bool Emulator::testing = false;

void Emulator::loadROM(const std::vector<Byte> &program)
{
	// define our instructions
	if (Memory::ROM_END - Memory::ROM_START + 1 < program.size())
	{
		std::cerr << "Cannot install ROM, too big." << std::endl;
	}

	std::memcpy(mem.memory + Memory::ROM_START, program.data(), program.size());
	std::cout << "Loaded ROM successfully." << std::endl;
}

void Emulator::run()
{
	while (cpu.program_counter < Memory::ROM_END)
	{
		if (!cycle())
		{
			break;
		}
	}
}

bool Emulator::cycle()
{
	int opcode = mem.readByte(cpu.program_counter);
	auto instruction = instruction_map[opcode];

	if (instruction.name == "DONE" || (!testing && instruction.name == "BRK"))
	{
		// invalid opcode, so get out of here asap
		return false;
	}

	instruction.implementation(opcode);
	cpu.program_counter++;
	
	// simulate the delay
	if (!testing)
	{
		DELAY_CYCLES(instruction_map, opcode);
	}

	return true;
}

void Emulator::handleArithmeticFlagChanges(Byte value)
{
	cpu.P &= ~MOS_6502::P_ZERO;
	cpu.P &= ~MOS_6502::P_NEGATIVE;

	if (value == 0)
	{
		cpu.P |= MOS_6502::P_ZERO;
	}

	if (IS_BIT_ON(value, 7))
	{
		cpu.P |= MOS_6502::P_NEGATIVE;
	}
}

Byte *Emulator::handleAddressing(int opcode)
{
	auto mode = instruction_map[opcode].addressing_mode;
	switch (mode)
	{
	case AddressMode::IMMEDIATE:
		return immediate();
	case AddressMode::ZERO_PAGE:
		return zeroPage();
	case AddressMode::ZERO_PAGE_AND_X:
		return zeroPageX();
	case AddressMode::ZERO_PAGE_AND_Y:
		return zeroPageY();
	case AddressMode::ABSOLUTE:
		return absolute();
	case AddressMode::ABSOLUTE_AND_X:
		return absoluteX();
	case AddressMode::ABSOLUTE_AND_Y:
		return absoluteY();
	case AddressMode::INDIRECT:
		return indirect();
	case AddressMode::INDEXED_INDIRECT:
		return indexedIndirect();
	case AddressMode::INDIRECT_INDEXED:
		return indirectIndexed();
	case AddressMode::ACCUMULATOR:
		return accumulator();
	case AddressMode::IMPLICIT:
		// No operand, return 0 or a dummy value
		return nullptr;
	case AddressMode::RELATIVE:
		return relative();
	default:
		std::cerr << "Unhandled addressing mode" << std::endl;
		return nullptr;
	}
}

Byte *Emulator::accumulator()
{
	return &cpu.accumulator;
}

Byte *Emulator::immediate()
{
	Word addr = ++cpu.program_counter;
	return mem.memory + addr;
}

Byte *Emulator::zeroPage()
{
	Byte offset = mem.readByte(++cpu.program_counter);
	return mem.memory + Word(offset);
}

Byte *Emulator::zeroPageX()
{
	Byte page_offset = mem.readByte(++cpu.program_counter);
	Byte offset = cpu.X;
	return mem.memory + Word((page_offset + offset) & 0xFF);
}

Byte *Emulator::zeroPageY()
{
	Byte page_offset = mem.readByte(++cpu.program_counter);
	Byte offset = cpu.Y;
	return mem.memory + Word((page_offset + offset) & 0xFF); // simulate zpg round behavior
}

Byte *Emulator::relative()
{
	// make sure to range limit the offset
	SignedByte offset = (SignedByte)mem.readByte(++cpu.program_counter);
	return mem.memory + Word(cpu.program_counter + offset);
}

/* spit out an address */
Byte *Emulator::absolute()
{
	Byte low = mem.readByte(Word(++cpu.program_counter));
	Byte high = mem.readByte(Word(++cpu.program_counter));
	Word addr = ((Word)low | ((Word)high << 8));
	return mem.memory + (addr & 0xFFFF);
}

Byte *Emulator::absoluteX()
{
	int offset = cpu.X;
	Byte low = mem.readByte(++cpu.program_counter);
	Byte high = mem.readByte(++cpu.program_counter);
	Word addr = ((Word)low | ((Word)high << 8));

	// if these are different pages, incur page penalty
	if ((addr & 0xFF00) != ((addr + offset) & 0xFF00))
	{
		delayMicros(CLOCK_uS);
	}

	return mem.memory + Word(addr + offset);
}

Byte *Emulator::absoluteY()
{
	int offset = cpu.Y;
	Byte low = mem.readByte(++cpu.program_counter);
	Byte high = mem.readByte(++cpu.program_counter);
	Word addr = ((Word)low | ((Word)high << 8));

	// if these are different pages, incur page penalty
	if ((addr & 0xFF00) != ((addr + offset) & 0xFF00))
	{
		delayMicros(CLOCK_uS);
	}

	return mem.memory + Word(addr + offset);
}

/* pointer to pointer */
Byte *Emulator::indirect()
{
	Byte lower_byte = mem.readByte(++cpu.program_counter);
	Byte higher_byte = mem.readByte(++cpu.program_counter);
	Word location = ((Word)higher_byte << 8) | (Word)lower_byte;
	Byte pointer_low = mem.readByte(location);
	Byte pointer_high;
	// simulate jump bug if crossing page boundary
	if (((location + 1) & 0xFF00) != (location & 0xFF00))
	{
		pointer_high = mem.readByte(location & 0xFF00);
	}
	else
	{
		pointer_high = mem.readByte(location + 1);
	}

	Word actual_location = ((Word)pointer_high << 8) | (Word)pointer_low;
	return mem.memory + actual_location;
}

/* (zp + x) */
Byte *Emulator::indexedIndirect()
{
	Byte offset = cpu.X;
	/* location in the zero page of the initial address*/
	Byte location = mem.readByte(++cpu.program_counter);
	/* The zero page address offsetted*/
	Byte zp_address = (Byte)(offset + location); // simulates a bug, making this effective only in the zero page
	Byte lower_byte = mem.readByte(zp_address);
	Byte higher_byte = mem.readByte((Byte)(zp_address + 1));
	Word target_address = ((Word)lower_byte | ((Word)higher_byte << 8));
	// return (zp + x)
	return mem.memory + target_address;
}

/* (zp) + y  */
Byte *Emulator::indirectIndexed()
{
	Byte offset = cpu.Y;
	Byte location = mem.readByte(++cpu.program_counter); // location in the zero page
	// get the address from the zero page
	Byte lower_byte = mem.readByte(location);
	Byte higher_byte = mem.readByte((Byte)(location + 1)); // implement wrap around bug for 6502
	Word target_address = ((Word)lower_byte | (Word)(higher_byte << 8));

	// incur page crossing penalty
	if ((target_address & 0xFF00) != ((target_address + offset) & 0xFF00))
	{
		if (!testing)
			delayMicros(CLOCK_uS);
	}

	// add offset to it
	return mem.memory + Word(target_address + offset); // simulate wrap around
}

/* do nothing */
void Emulator::NOP(int opcode)
{
	((void)sizeof(char[1 - 2 * !!(printf(""))]));
}

void Emulator::ORA(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.accumulator |= *addr;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::INX(int opcode)
{
	cpu.X++;
	handleArithmeticFlagChanges(cpu.X);
}

void Emulator::INY(int opcode)
{
	cpu.Y++;
	handleArithmeticFlagChanges(cpu.Y);
}

void Emulator::DEX(int opcode)
{
	cpu.X--;
	handleArithmeticFlagChanges(cpu.X);
}

void Emulator::DEY(int opcode)
{
	cpu.Y--;
	handleArithmeticFlagChanges(cpu.Y);
}

void Emulator::INC(int opcode)
{
	Byte *location = handleAddressing(opcode);
	(*location)++;
	handleArithmeticFlagChanges(*location);
}

void Emulator::DEC(int opcode)
{
	Byte *location = handleAddressing(opcode);
	(*location)--;
	handleArithmeticFlagChanges(*location);
}

void Emulator::BRK(int opcode)
{
	// simulate pad byte
	cpu.program_counter++;
	mem.stackPushWord(cpu.S, cpu.program_counter + 1);                        // Push PC + 1
	mem.stackPushByte(cpu.S, cpu.P | MOS_6502::P_BREAK | MOS_6502::P_UNUSED); // Push status

	// disable interrupts (we are one)
	cpu.P |= MOS_6502::P_INT_DISABLE;

	Byte low = mem.readByte(0xFFFE);
	Byte high = mem.readByte(0xFFFF);
	cpu.program_counter = (((Word)high << 8) | (Word)low) - 1;
}

void Emulator::TXA(int opcode)
{
	cpu.accumulator = cpu.X;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::TAY(int opcode)
{
	cpu.Y = cpu.accumulator;
	handleArithmeticFlagChanges(cpu.Y);
}

void Emulator::TSX(int opcode)
{
	cpu.X = cpu.S;
	handleArithmeticFlagChanges(cpu.X);
}

void Emulator::TXS(int opcode)
{
	cpu.S = cpu.X;
	// handleArithmeticFlagChanges(cpu.S); bro what??
}

void Emulator::TYA(int opcode)
{
	cpu.accumulator = cpu.Y;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::TAX(int opcode)
{
	cpu.X = cpu.accumulator;
	handleArithmeticFlagChanges(cpu.X);
}

void Emulator::LDA(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.accumulator = *addr;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::LDX(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.X = *addr;
	handleArithmeticFlagChanges(cpu.X);
}

void Emulator::LDY(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.Y = *addr;
	handleArithmeticFlagChanges(cpu.Y);
}

void Emulator::STA(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	*addr = cpu.accumulator;
}

void Emulator::STX(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	*addr = cpu.X;
}

void Emulator::STY(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	*addr = cpu.Y;
}

void Emulator::JMP(int opcode)
{
	Byte *location = handleAddressing(opcode);
	// jump to that memory location (waoh)
	cpu.program_counter = (Word)(location - mem.memory - 1);
}

void Emulator::JSR(int opcode)
{
	Byte *location = handleAddressing(opcode);
	mem.stackPushWord(cpu.S, cpu.program_counter);         // push the return address (pc has already been incremented by addr func)
	cpu.program_counter = Word(location - mem.memory - 1); // -1 for off by one  i guess
}

void Emulator::RTS(int opcode)
{
	Word return_address = mem.stackPullWord(cpu.S);
	cpu.program_counter = return_address;
}

/* TODO: UPDATE THE CODE TO SET THE B FLAG once we know about register sttuff*/
void Emulator::PHA(int opcode)
{
	mem.stackPushByte(cpu.S, cpu.accumulator);
}

void Emulator::PLA(int opcode)
{
	cpu.accumulator = mem.stackPullByte(cpu.S);
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::PHP(int opcode)
{
	mem.stackPushByte(cpu.S, cpu.P | MOS_6502::P_BREAK | MOS_6502::P_UNUSED); // must always be set
}

void Emulator::PLP(int opcode)
{
	cpu.P = mem.stackPullByte(cpu.S);
	cpu.P &= ~MOS_6502::P_BREAK; // Clear B (this is some internal detail i suppose)
	cpu.P |= MOS_6502::P_UNUSED; // set the unused bit just in case
}

void Emulator::AND(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.accumulator &= *addr;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::EOR(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	cpu.accumulator ^= *addr;
	handleArithmeticFlagChanges(cpu.accumulator);
}

void Emulator::initInstructionMap()
{
	for (auto &i : instruction_map)
	{
		i = {"DONE", 0x02, 1, 1, AddressMode::IMPLICIT, nullptr}; // nullptr for implementation as it's a custom termination
	}

	auto MAKE_BINDING = [&](void (Emulator::*member_func_ptr)(int))
	{
		return std::function<void(Byte)>(std::bind(member_func_ptr, this, _1));
	};

	instruction_map[0x70] = {"BVS", 0x70, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BVS)};
	instruction_map[0x00] = {"BRK", 0x00, 1, 7, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::BRK)};
	instruction_map[0xC9] = {"CMP", 0xC9, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xC5] = {"CMP", 0xC5, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xD5] = {"CMP", 0xD5, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xCD] = {"CMP", 0xCD, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xDD] = {"CMP", 0xDD, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xD9] = {"CMP", 0xD9, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xC1] = {"CMP", 0xC1, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xD1] = {"CMP", 0xD1, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::CMP)};
	instruction_map[0xE0] = {"CPX", 0xE0, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::CPX)};
	instruction_map[0xE4] = {"CPX", 0xE4, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::CPX)};
	instruction_map[0xEC] = {"CPX", 0xEC, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::CPX)};
	instruction_map[0xC0] = {"CPY", 0xC0, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::CPY)};
	instruction_map[0xC4] = {"CPY", 0xC4, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::CPY)};
	instruction_map[0xCC] = {"CPY", 0xCC, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::CPY)};
	instruction_map[0xCA] = {"DEX", 0xCA, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::DEX)};
	instruction_map[0x88] = {"DEY", 0x88, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::DEY)};
	instruction_map[0x49] = {"EOR", 0x49, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x45] = {"EOR", 0x45, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x55] = {"EOR", 0x55, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x4D] = {"EOR", 0x4D, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x5D] = {"EOR", 0x5D, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x59] = {"EOR", 0x59, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x41] = {"EOR", 0x41, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0x51] = {"EOR", 0x51, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::EOR)};
	instruction_map[0xE8] = {"INX", 0xE8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::INX)};
	instruction_map[0xC8] = {"INY", 0xC8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::INY)};
	instruction_map[0x4C] = {"JMP", 0x4C, 3, 3, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::JMP)};
	instruction_map[0x6C] = {"JMP", 0x6C, 3, 5, AddressMode::INDIRECT, MAKE_BINDING(&Emulator::JMP)};
	instruction_map[0x20] = {"JSR", 0x20, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::JSR)};
	instruction_map[0xA9] = {"LDA", 0xA9, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xA5] = {"LDA", 0xA5, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xB5] = {"LDA", 0xB5, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xAD] = {"LDA", 0xAD, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xBD] = {"LDA", 0xBD, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xB9] = {"LDA", 0xB9, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xA1] = {"LDA", 0xA1, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xB1] = {"LDA", 0xB1, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::LDA)};
	instruction_map[0xA2] = {"LDX", 0xA2, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::LDX)};
	instruction_map[0xA6] = {"LDX", 0xA6, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::LDX)};
	instruction_map[0xB6] = {"LDX", 0xB6, 2, 4, AddressMode::ZERO_PAGE_AND_Y, MAKE_BINDING(&Emulator::LDX)};
	instruction_map[0xAE] = {"LDX", 0xAE, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::LDX)};
	instruction_map[0xBE] = {"LDX", 0xBE, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::LDX)};
	instruction_map[0xA0] = {"LDY", 0xA0, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::LDY)};
	instruction_map[0xA4] = {"LDY", 0xA4, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::LDY)};
	instruction_map[0xB4] = {"LDY", 0xB4, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::LDY)};
	instruction_map[0xAC] = {"LDY", 0xAC, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::LDY)};
	instruction_map[0xBC] = {"LDY", 0xBC, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::LDY)};
	instruction_map[0x4A] = {"LSR", 0x4A, 1, 2, AddressMode::ACCUMULATOR, MAKE_BINDING(&Emulator::LSR)};
	instruction_map[0x46] = {"LSR", 0x46, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::LSR)};
	instruction_map[0x56] = {"LSR", 0x56, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::LSR)};
	instruction_map[0x4E] = {"LSR", 0x4E, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::LSR)};
	instruction_map[0x5E] = {"LSR", 0x5E, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::LSR)};
	instruction_map[0xEA] = {"NOP", 0xEA, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::NOP)};
	instruction_map[0x09] = {"ORA", 0x09, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x05] = {"ORA", 0x05, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x15] = {"ORA", 0x15, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x0D] = {"ORA", 0x0D, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x1D] = {"ORA", 0x1D, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x19] = {"ORA", 0x19, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x01] = {"ORA", 0x01, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::ORA)};
	instruction_map[0x11] = {"ORA", 0x11, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::ORA)};
	// STA - Store Accumulator
	instruction_map[0x85] = {"STA", 0x85, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::STA)};
	instruction_map[0x95] = {"STA", 0x95, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::STA)};
	instruction_map[0x8D] = {"STA", 0x8D, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::STA)};
	instruction_map[0x9D] = {"STA", 0x9D, 3, 5, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::STA)};
	instruction_map[0x99] = {"STA", 0x99, 3, 5, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::STA)};
	instruction_map[0x81] = {"STA", 0x81, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::STA)}; // (Indirect,X)
	instruction_map[0x91] = {"STA", 0x91, 2, 6, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::STA)}; // (Indirect),Y
	// ADC - Add with Carry
	instruction_map[0x69] = {"ADC", 0x69, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x65] = {"ADC", 0x65, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x75] = {"ADC", 0x75, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x6D] = {"ADC", 0x6D, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x7D] = {"ADC", 0x7D, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x79] = {"ADC", 0x79, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x61] = {"ADC", 0x61, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::ADC)};
	instruction_map[0x71] = {"ADC", 0x71, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::ADC)};

	// SBC - Subtract with Carry
	instruction_map[0xE9] = {"SBC", 0xE9, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xE5] = {"SBC", 0xE5, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xF5] = {"SBC", 0xF5, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xED] = {"SBC", 0xED, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xFD] = {"SBC", 0xFD, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xF9] = {"SBC", 0xF9, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xE1] = {"SBC", 0xE1, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xF1] = {"SBC", 0xF1, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::SBC)};
	instruction_map[0xAA] = {"TAX", 0xAA, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TAX)};
	instruction_map[0x8A] = {"TXA", 0x8A, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TXA)};
	instruction_map[0xA8] = {"TAY", 0xA8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TAY)};
	instruction_map[0x98] = {"TYA", 0x98, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TYA)};
	instruction_map[0x0A] = {"ASL", 0x0A, 1, 2, AddressMode::ACCUMULATOR, MAKE_BINDING(&Emulator::ASL)};
	instruction_map[0x06] = {"ASL", 0x06, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::ASL)};
	instruction_map[0x16] = {"ASL", 0x16, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::ASL)};
	instruction_map[0x0E] = {"ASL", 0x0E, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::ASL)};
	instruction_map[0x1E] = {"ASL", 0x1E, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::ASL)};
	instruction_map[0x2A] = {"ROL", 0x2A, 1, 2, AddressMode::ACCUMULATOR, MAKE_BINDING(&Emulator::ROL)};
	instruction_map[0x26] = {"ROL", 0x26, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::ROL)};
	instruction_map[0x36] = {"ROL", 0x36, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::ROL)};
	instruction_map[0x2E] = {"ROL", 0x2E, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::ROL)};
	instruction_map[0x3E] = {"ROL", 0x3E, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::ROL)};
	instruction_map[0x6A] = {"ROR", 0x6A, 1, 2, AddressMode::ACCUMULATOR, MAKE_BINDING(&Emulator::ROR)};
	instruction_map[0x66] = {"ROR", 0x66, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::ROR)};
	instruction_map[0x76] = {"ROR", 0x76, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::ROR)};
	instruction_map[0x6E] = {"ROR", 0x6E, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::ROR)};
	instruction_map[0x7E] = {"ROR", 0x7E, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::ROR)};
	instruction_map[0x18] = {"CLC", 0x18, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::CLC)};
	instruction_map[0xD8] = {"CLD", 0xD8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::CLD)};
	instruction_map[0x58] = {"CLI", 0x58, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::CLI)};
	instruction_map[0xB8] = {"CLV", 0xB8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::CLV)};
	;
	instruction_map[0x38] = {"SEC", 0x38, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::SEC)};
	instruction_map[0xF8] = {"SED", 0xF8, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::SED)};
	instruction_map[0x29] = {"AND", 0x29, 2, 2, AddressMode::IMMEDIATE, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x25] = {"AND", 0x25, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x35] = {"AND", 0x35, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x2D] = {"AND", 0x2D, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x3D] = {"AND", 0x3D, 3, 4, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x39] = {"AND", 0x39, 3, 4, AddressMode::ABSOLUTE_AND_Y, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x21] = {"AND", 0x21, 2, 6, AddressMode::INDEXED_INDIRECT, MAKE_BINDING(&Emulator::AND)};
	instruction_map[0x31] = {"AND", 0x31, 2, 5, AddressMode::INDIRECT_INDEXED, MAKE_BINDING(&Emulator::AND)};

	// BIT
	instruction_map[0x24] = {"BIT", 0x24, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::BIT)};
	instruction_map[0x2C] = {"BIT", 0x2C, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::BIT)};

	// Branches
	instruction_map[0x90] = {"BCC", 0x90, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BCC)};
	instruction_map[0xB0] = {"BCS", 0xB0, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BCS)};
	instruction_map[0xF0] = {"BEQ", 0xF0, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BEQ)};
	instruction_map[0x30] = {"BMI", 0x30, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BMI)};
	instruction_map[0xD0] = {"BNE", 0xD0, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BNE)};
	instruction_map[0x10] = {"BPL", 0x10, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BPL)};
	instruction_map[0x50] = {"BVC", 0x50, 2, 2, AddressMode::RELATIVE, MAKE_BINDING(&Emulator::BVC)};

	// DEC
	instruction_map[0xC6] = {"DEC", 0xC6, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::DEC)};
	instruction_map[0xD6] = {"DEC", 0xD6, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::DEC)};
	instruction_map[0xCE] = {"DEC", 0xCE, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::DEC)};
	instruction_map[0xDE] = {"DEC", 0xDE, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::DEC)};

	// INC
	instruction_map[0xE6] = {"INC", 0xE6, 2, 5, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::INC)};
	instruction_map[0xF6] = {"INC", 0xF6, 2, 6, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::INC)};
	instruction_map[0xEE] = {"INC", 0xEE, 3, 6, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::INC)};
	instruction_map[0xFE] = {"INC", 0xFE, 3, 7, AddressMode::ABSOLUTE_AND_X, MAKE_BINDING(&Emulator::INC)};

	// RTI & RTS
	instruction_map[0x40] = {"RTI", 0x40, 1, 6, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::RTI)};
	instruction_map[0x60] = {"RTS", 0x60, 1, 6, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::RTS)};

	// STX
	instruction_map[0x86] = {"STX", 0x86, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::STX)};
	instruction_map[0x96] = {"STX", 0x96, 2, 4, AddressMode::ZERO_PAGE_AND_Y, MAKE_BINDING(&Emulator::STX)};
	instruction_map[0x8E] = {"STX", 0x8E, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::STX)};

	// STY
	instruction_map[0x84] = {"STY", 0x84, 2, 3, AddressMode::ZERO_PAGE, MAKE_BINDING(&Emulator::STY)};
	instruction_map[0x94] = {"STY", 0x94, 2, 4, AddressMode::ZERO_PAGE_AND_X, MAKE_BINDING(&Emulator::STY)};
	instruction_map[0x8C] = {"STY", 0x8C, 3, 4, AddressMode::ABSOLUTE, MAKE_BINDING(&Emulator::STY)};

	// TSX / TXS
	instruction_map[0xBA] = {"TSX", 0xBA, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TSX)};
	instruction_map[0x9A] = {"TXS", 0x9A, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::TXS)};
	instruction_map[0x48] = {"PHA", 0x48, 1, 3, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::PHA)};
	instruction_map[0x08] = {"PHP", 0x08, 1, 3, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::PHP)};
	instruction_map[0x68] = {"PLA", 0x68, 1, 4, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::PLA)};
	instruction_map[0x28] = {"PLP", 0x28, 1, 4, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::PLP)};
	instruction_map[0x78] = {"SEI", 0x78, 1, 2, AddressMode::IMPLICIT, MAKE_BINDING(&Emulator::SEI)};

	// Custom end-of-program instruction
	instruction_map[0x02] = {"DONE", 0xFE, 1, 1, AddressMode::IMPLICIT, [](int) {}}; // nullptr for implementation as it's a custom termination
}

void Emulator::CLC(int opcode)
{
	cpu.P &= ~MOS_6502::P_CARRY;
}

void Emulator::CLD(int opcode)
{
	cpu.P &= ~MOS_6502::P_DECIMAL;
}

void Emulator::CLI(int opcode)
{
	cpu.P &= ~MOS_6502::P_INT_DISABLE;
}

void Emulator::CLV(int opcode)
{
	cpu.P &= ~MOS_6502::P_OVERFLOW;
}

void Emulator::SEC(int opcode)
{
	cpu.P |= MOS_6502::P_CARRY;
}

void Emulator::SED(int opcode)
{
	cpu.P |= MOS_6502::P_DECIMAL;
}

void Emulator::SEI(int opcode)
{
	cpu.P |= MOS_6502::P_INT_DISABLE;
}

void Emulator::RTI(int opcode)
{
	// get our status restored
	cpu.P = mem.stackPullByte(cpu.S);
	cpu.P &= ~MOS_6502::P_BREAK;
	// this should be set to 1 all times
	cpu.P |= MOS_6502::P_UNUSED;

	cpu.program_counter = mem.stackPullWord(cpu.S) - 1;
}

void Emulator::ADC(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Word carry = (cpu.P & MOS_6502::P_CARRY) ? 1 : 0;
	Word result = (Word)cpu.accumulator + *addr + carry;
	Byte result8 = result & 0xFF;
	// what the cryptic bro :skull:
	int is_overflow = (~(cpu.accumulator ^ *addr) & (cpu.accumulator ^ result)) & 0x80;
	cpu.accumulator = result8;

	if (result >= 0x100) // if the result is greater than 8 bits, ensue destruction upon the world
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	if (is_overflow)
	{
		cpu.P |= MOS_6502::P_OVERFLOW;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_OVERFLOW;
	}

	handleArithmeticFlagChanges(result8);
}

void Emulator::SBC(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Word carry = (cpu.P & MOS_6502::P_CARRY) ? 1 : 0;
	Word result = (Word)cpu.accumulator - *addr - (1 - carry);
	Byte result8 = result & 0xFF;

	int is_overflow = ((cpu.accumulator ^ *addr) & (cpu.accumulator ^ (Byte)result)) & 0x80;

	cpu.accumulator = result8;

	if (result < 0x100)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	if (is_overflow)
	{
		cpu.P |= MOS_6502::P_OVERFLOW;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_OVERFLOW;
	}

	handleArithmeticFlagChanges(result8);
}

void Emulator::ASL(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Byte should_carry = 0x80 & *addr;
	*addr = ((*addr << 1) & 0xFF); // shift one right and maintain 8 bits

	if (should_carry)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	handleArithmeticFlagChanges(*addr);
}

void Emulator::LSR(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Byte should_carry = *addr & 1; // least significant bit
	*addr = ((*addr >> 1) & 0xFF);

	if (should_carry)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	handleArithmeticFlagChanges(*addr);
}

void Emulator::ROL(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Byte should_carry = *addr & 0x80;
	Byte _0_bit_val = (cpu.P & MOS_6502::P_CARRY) ? 1 : 0;
	*addr = (*addr << 1) | (_0_bit_val);

	if (should_carry)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	handleArithmeticFlagChanges(*addr);
}

void Emulator::ROR(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Byte original_bit0 = *addr & 0x01;
	Byte carry_in = (cpu.P & MOS_6502::P_CARRY) ? 0x80 : 0;
	*addr = (*addr >> 1) | carry_in; // 7th bit

	if (original_bit0)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	handleArithmeticFlagChanges(*addr);
}

void Emulator::CMP(int opcode)
{
	Byte *addr = handleAddressing(opcode);

	// Compare: A - M
	Byte A = cpu.accumulator;
	Byte M = *addr;
	Byte result = A - M;

	if (A == M)
	{
		cpu.P |= MOS_6502::P_ZERO;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_ZERO;
	}

	if (A >= M)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	// Set Negative flag if bit 7 of (A - M) is set
	if (result & 0x80)
	{
		cpu.P |= MOS_6502::P_NEGATIVE;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_NEGATIVE;
	}
}

void Emulator::CPX(int opcode)
{
	Byte *addr = handleAddressing(opcode);

	// Compare: X - M
	Byte X = cpu.X;
	Byte M = *addr;
	Byte result = X - M;

	if (X == M)
	{
		cpu.P |= MOS_6502::P_ZERO;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_ZERO;
	}

	if (X >= M)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	// Set Negative flag if bit 7 of (A - M) is set
	if (result & 0x80)
	{
		cpu.P |= MOS_6502::P_NEGATIVE;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_NEGATIVE;
	}
}

void Emulator::CPY(int opcode)
{
	Byte *addr = handleAddressing(opcode);

	Byte Y = cpu.Y;
	Byte M = *addr;
	Byte result = Y - M;

	if (Y == M)
	{
		cpu.P |= MOS_6502::P_ZERO;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_ZERO;
	}

	if (Y >= M)
	{
		cpu.P |= MOS_6502::P_CARRY;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_CARRY;
	}

	// Set Negative flag if bit 7 of (A - M) is set
	if (result & 0x80)
	{
		cpu.P |= MOS_6502::P_NEGATIVE;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_NEGATIVE;
	}
}

void Emulator::branchIf(bool condition, Byte *to)
{
	if (condition)
	{
		size_t location = to - mem.memory;
		cpu.program_counter = (Word)location;
	}
}

void Emulator::BCC(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf((cpu.P & MOS_6502::P_CARRY) == 0, offset);
}

void Emulator::BCS(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf(cpu.P & MOS_6502::P_CARRY, offset);
}

void Emulator::BEQ(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf(cpu.P & MOS_6502::P_ZERO, offset);
}

void Emulator::BMI(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf(cpu.P & MOS_6502::P_NEGATIVE, offset);
}

void Emulator::BNE(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf((cpu.P & MOS_6502::P_ZERO) == 0, offset);
}

void Emulator::BPL(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf((cpu.P & MOS_6502::P_NEGATIVE) == 0, offset);
}

void Emulator::BVC(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf((cpu.P & MOS_6502::P_OVERFLOW) == 0, offset);
}

void Emulator::BVS(int opcode)
{
	Byte *offset = handleAddressing(opcode);
	branchIf(cpu.P & MOS_6502::P_OVERFLOW, offset);
}

void Emulator::BIT(int opcode)
{
	Byte *addr = handleAddressing(opcode);
	Byte value = *addr;
	Byte result = cpu.accumulator & value;

	// Set or clear zero flag based on accumulator & memory
	if (result == 0)
	{
		cpu.P |= MOS_6502::P_ZERO;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_ZERO;
	}

	// Set Negative flag to bit 7 of memory operand
	if (value & 0x80)
	{
		cpu.P |= MOS_6502::P_NEGATIVE;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_NEGATIVE;
	}

	// Set Overflow flag to bit 6 of memory operand
	if (value & 0x40)
	{
		cpu.P |= MOS_6502::P_OVERFLOW;
	}
	else
	{
		cpu.P &= ~MOS_6502::P_OVERFLOW;
	}
}
