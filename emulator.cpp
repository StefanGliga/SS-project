#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <iomanip>
#include <format>
#include <chrono>

#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

using namespace std;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;


std::vector<u8> memory;

struct cpu
{
    u32 gpr[16];
    u32 csr[3];
};

struct instr_info
{
    u8 mode: 4;
    u8 opcode: 4;
    u8 b: 4;
    u8 a: 4;
    u8 d3: 4;
    u8 c: 4;
    u8 d1: 4;
    u8 d2: 4;
};

union instr
{
    u32 raw;
    instr_info info;
};

void push(cpu& cpu, u32 value)
{
    cpu.gpr[14] -= 4;
    *(u32*)(memory.data() + cpu.gpr[14]) = value;
}

u32 pop(cpu& cpu)
{
    u32 value = *(u32*)(memory.data() + cpu.gpr[14]);
    cpu.gpr[14] += 4;
    return value;
}

void interrupt(cpu& cpu, u32 cause)
{
    // push psw and pc to stack
    push(cpu, cpu.csr[0]);
    push(cpu, cpu.gpr[15]);
    // disable interrupts and jump to interrupt handler with cause
    cpu.csr[0] |= 4;
    cpu.csr[2] = cause;
    cpu.gpr[15] = cpu.csr[1];
}

auto getdur()
{
    u32 tim_cfg = *(u32*)(memory.data() + 0xFFFFFF10);
    switch(tim_cfg)
    {
        case 0x0:
            return chrono::milliseconds(500);
        case 0x1:
            return chrono::milliseconds(1000);
        case 0x2:
            return chrono::milliseconds(1500);
        case 0x3:
            return chrono::milliseconds(2000);
        case 0x4:
            return chrono::milliseconds(5000);
        case 0x5:
            return chrono::milliseconds(10000);
        case 0x6:
            return chrono::milliseconds(30000);
        case 0x7:
            return chrono::milliseconds(60000);
        default:
            return chrono::milliseconds(500);
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        cout << "Usage: emulator <input_file>" << endl;
        return 1;
    }

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    memory.resize(1ull << 32);

    ifstream file(argv[1]);
    if (!file.is_open())
    {
        cout << "Could not open file: " << argv[1] << endl;
        return 1;
    }

    file.read((char*)memory.data(), (streamsize)memory.size());
    file.close();

    cpu cpu{};
    cpu.gpr[15] = 0x40000000;
    *(u32*)(memory.data() + 0xFFFFFF10) = 0x0; // timer config

    bool timer_interrupt_pending = false;
    bool keyboard_interrupt_pending = false;
    auto time_of_last_timer_intr_handling = chrono::steady_clock::now();

    bool running = true;
    while(running) [[likely]]
    {
        //u32 instr = memory[cpu.gpr[15]];
        u32 instruction = *(u32*)(memory.data() + cpu.gpr[15]);
        instr i;
        i.raw = instruction;

        i16 D = i.info.d1 | (i.info.d2 << 4) | (i.info.d3 << 8);
        // sign extend from 12 bits
        if (D & 0x800)
        {
            D |= 0xF000;
        }

        cpu.gpr[15] += 4;

        switch (i.info.opcode)
        {
            case 0:{
                running = false;
                break;
            }
            case 1:{
                interrupt(cpu, 4);
                break;
            }
            case 2:{
                // call
                push(cpu, cpu.gpr[15]);
                u32 tmp = cpu.gpr[i.info.a] + cpu.gpr[i.info.b] + D;
                switch (i.info.mode)
                {
                    case 0:{
                        cpu.gpr[15] = tmp;
                        break;
                    }
                    case 1:{
                        cpu.gpr[15] = *(u32*)(memory.data() + tmp);
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 3:{
                // jmp
                u32 tmp = cpu.gpr[i.info.a] + D;

                switch(i.info.mode)
                {
                    case 0:{
                        cpu.gpr[15] = tmp;
                        break;
                    }
                    case 1:{
                        if (cpu.gpr[i.info.b] == cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = tmp;
                        }
                        break;
                    }
                    case 2:{
                        if (cpu.gpr[i.info.b] != cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = tmp;
                        }
                        break;
                    }
                    case 3:{
                        if ((i32)cpu.gpr[i.info.b] > (i32)cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = tmp;
                        }
                        break;
                    }
                    case 8:{
                        cpu.gpr[15] = *(u32*)(memory.data() + tmp);
                        break;
                    }
                    case 9:{
                        if (cpu.gpr[i.info.b] == cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = *(u32*)(memory.data() + tmp);
                        }
                        break;
                    }
                    case 10:{
                        if (cpu.gpr[i.info.b] != cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = *(u32*)(memory.data() + tmp);
                        }
                        break;
                    }
                    case 11:{
                        if ((i32)cpu.gpr[i.info.b] > (i32)cpu.gpr[i.info.c])
                        {
                            cpu.gpr[15] = *(u32*)(memory.data() + tmp);
                        }
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 4: {
                // xchg
                u32 tmp = cpu.gpr[i.info.c];
                cpu.gpr[i.info.c] = cpu.gpr[i.info.b];
                cpu.gpr[i.info.b] = tmp;
                break;
            }
            case 5: {
                // arith (+,-,*,/)
                switch (i.info.mode)
                {
                    case 0:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] + cpu.gpr[i.info.c];
                        break;
                    }
                    case 1:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] - cpu.gpr[i.info.c];
                        break;
                    }
                    case 2:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] * cpu.gpr[i.info.c];
                        break;
                    }
                    case 3:{
                        if (cpu.gpr[i.info.c] == 0)
                        {
                            interrupt(cpu, 1);
                        }
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] / cpu.gpr[i.info.c];
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 6: {
                // logic (~, &, |, ^)
                switch (i.info.mode)
                {
                    case 0:{
                        cpu.gpr[i.info.a] = ~cpu.gpr[i.info.b];
                        break;
                    }
                    case 1:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] & cpu.gpr[i.info.c];
                        break;
                    }
                    case 2:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] | cpu.gpr[i.info.c];
                        break;
                    }
                    case 3:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] ^ cpu.gpr[i.info.c];
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 7: {
                // shift (<<, >>)
                switch (i.info.mode)
                {
                    case 0:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] << cpu.gpr[i.info.c];
                        break;
                    }
                    case 1:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] >> cpu.gpr[i.info.c];
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 8: {
                // store
                u32 tmp = cpu.gpr[i.info.a] + cpu.gpr[i.info.b] + D;
                switch (i.info.mode)
                {
                    case 0:{
                        *(u32*)(memory.data() + tmp) = cpu.gpr[i.info.c];
                        break;
                    }
                    case 1:{
                        cpu.gpr[i.info.a] += D;
                        *(u32*)(memory.data() + cpu.gpr[i.info.a]) = cpu.gpr[i.info.c];
                        break;
                    }
                    case 2:{
                        *(u32*)(memory.data() + *(u32*)(memory.data() + tmp)) = cpu.gpr[i.info.c];
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            case 9: {
                //load
                u32 tmp = cpu.gpr[i.info.b] + cpu.gpr[i.info.c] + D;
                switch (i.info.mode)
                {
                    case 0:{
                        cpu.gpr[i.info.a] = cpu.csr[i.info.b];
                        break;
                    }
                    case 1:{
                        cpu.gpr[i.info.a] = cpu.gpr[i.info.b] + D;
                        break;
                    }
                    case 2:{
                        cpu.gpr[i.info.a] = *(u32*)(memory.data() + tmp);
                        break;
                    }
                    case 3:{
                        cpu.gpr[i.info.a] = *(u32*)(memory.data() + cpu.gpr[i.info.b]);
                        cpu.gpr[i.info.b] += D;
                        break;
                    }
                    case 4:{
                        cpu.csr[i.info.a] = cpu.gpr[i.info.b];
                        break;
                    }
                    case 5:{
                        cpu.csr[i.info.a] = cpu.csr[i.info.b] | D;
                        break;
                    }
                    case 6:{
                        cpu.csr[i.info.a] = *(u32*)(memory.data() + tmp);
                        break;
                    }
                    case 7:{
                        cpu.csr[i.info.a] = *(u32*)(memory.data() + cpu.gpr[i.info.b]);
                        cpu.gpr[i.info.b] += D;
                        break;
                    }
                    default:
                        interrupt(cpu, 1);
                }
                break;
            }
            default:
                interrupt(cpu, 1);
        }

        // interrupt handling
        int ch = getchar();
        if(ch != EOF)
        {
            keyboard_interrupt_pending = true;
            *(u32*)(memory.data() + 0xFFFFFF04) = ch;
        }
        ch = *(u32*)(memory.data() + 0xFFFFFF00);
        if(ch != EOF)
        {
            putchar(ch);
            *(u32*)(memory.data() + 0xFFFFFF00) = EOF;
        }

        auto time_since_last_interrupt = chrono::steady_clock::now() - time_of_last_timer_intr_handling;
        if(time_since_last_interrupt > getdur())
        {
            timer_interrupt_pending = true;
        }
        
        // check if interrupts are not masked
        if(not (cpu.csr[0] & 4))
        {
            if(not cpu.csr[1]) continue; // no interrupt handler set
            if(not (cpu.csr[0] & 1))
            {
                if(timer_interrupt_pending)
                {
                    interrupt(cpu, 2);
                    timer_interrupt_pending = false;
                    time_of_last_timer_intr_handling = chrono::steady_clock::now();
                }
            }
            if(not (cpu.csr[0] & 2))
            {
                if(keyboard_interrupt_pending)
                {
                    interrupt(cpu, 3);
                    keyboard_interrupt_pending = false;
                }
            }
        }
    }

    cout << "Emulated processor executed halt instruction" << endl;
    cout << "Emulated processor state:" << endl;
    for (int i = 0; i < 16; i++)
    {
        cout << format("r{}={:#010x} ", i, cpu.gpr[i]);
        if (i % 4 == 3)
        {
            cout << endl;
        }
    }

    return 0;

}