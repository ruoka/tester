#include <elf.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::string path = argc > 1 ? std::string{argv[1]} : std::string{"core"};
    std::ifstream file(path, std::ios::binary);
    if(!file)
    {
        std::cerr << "failed to open core file\n";
        return 1;
    }
    Elf64_Ehdr ehdr{};
    file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if(!file)
    {
        std::cerr << "failed to read ELF header\n";
        return 1;
    }
    file.seekg(ehdr.e_phoff, std::ios::beg);
    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    for(auto& ph : phdrs)
    {
        file.read(reinterpret_cast<char*>(&ph), sizeof(Elf64_Phdr));
    }
    auto read_at = [&](Elf64_Off offset, std::size_t size)
    {
        std::vector<char> buffer(size);
        file.seekg(offset, std::ios::beg);
        file.read(buffer.data(), size);
        return buffer;
    };
    uint64_t pc = 0;
    uint64_t sp = 0;
    for(const auto& ph : phdrs)
    {
        if(ph.p_type != PT_NOTE)
            continue;
        auto data = read_at(ph.p_offset, ph.p_filesz);
        std::size_t offset = 0;
        while(offset + sizeof(Elf64_Nhdr) <= data.size())
        {
            auto nhdr = reinterpret_cast<const Elf64_Nhdr*>(data.data() + offset);
            offset += sizeof(Elf64_Nhdr);
            auto align = [](std::size_t value){ return (value + 3u) & ~std::size_t(3u); };
            offset += align(nhdr->n_namesz);
            auto desc = data.data() + offset;
            offset += align(nhdr->n_descsz);
            if(nhdr->n_type == NT_PRSTATUS)
            {
                auto qwords = nhdr->n_descsz / sizeof(uint64_t);
                const uint64_t* gregs = reinterpret_cast<const uint64_t*>(desc + nhdr->n_descsz - 34*sizeof(uint64_t));
                sp = gregs[31];
                pc = gregs[32];
                for(int i = 0; i < 34; ++i)
                    std::cout << "greg[" << std::hex << i << "]=0x" << gregs[i] << "\n";
                std::cout << "pc=0x" << std::hex << pc << "\n";
                std::cout << "sp=0x" << std::hex << sp << "\n";
                return 0;
            }
        }
    }
    std::cerr << "prstatus not found\n";
    return 1;
}
