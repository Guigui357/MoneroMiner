#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include "bytecode_machine.hpp"
#include "reciprocal.h"

namespace randomx {
namespace {

static void uleb(std::vector<uint8_t>& o, uint32_t v) {
    do { uint8_t b = static_cast<uint8_t>(v & 0x7fu); v >>= 7; if (v) b |= 0x80u; o.push_back(b); } while (v);
}
static void sleb(std::vector<uint8_t>& o, int64_t v) {
    bool more = true;
    while (more) {
        uint8_t b = static_cast<uint8_t>(v & 0x7f);
        bool sign = (b & 0x40) != 0;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more) b |= 0x80;
        o.push_back(b);
    }
}
static void bytes(std::vector<uint8_t>& o, const char* s, size_t n) {
    uleb(o, static_cast<uint32_t>(n)); o.insert(o.end(), s, s + n);
}
static void section(std::vector<uint8_t>& m, uint8_t id, const std::vector<uint8_t>& p) {
    m.push_back(id); uleb(m, static_cast<uint32_t>(p.size())); m.insert(m.end(), p.begin(), p.end());
}
static void get(std::vector<uint8_t>& c, uint32_t i) { c.push_back(0x20); uleb(c, i); }
static void set(std::vector<uint8_t>& c, uint32_t i) { c.push_back(0x21); uleb(c, i); }
static void i32c(std::vector<uint8_t>& c, int32_t v) { c.push_back(0x41); sleb(c, v); }
static void i64c(std::vector<uint8_t>& c, int64_t v) { c.push_back(0x42); sleb(c, v); }
static void i64load(std::vector<uint8_t>& c) { c.push_back(0x29); uleb(c, 3); uleb(c, 0); }
static void i64store(std::vector<uint8_t>& c) { c.push_back(0x37); uleb(c, 3); uleb(c, 0); }
static void vload(std::vector<uint8_t>& c) { c.push_back(0xfd); uleb(c, 0); uleb(c, 4); uleb(c, 0); }
static void vstore(std::vector<uint8_t>& c) { c.push_back(0xfd); uleb(c, 0x0b); uleb(c, 4); uleb(c, 0); }
static void vop(std::vector<uint8_t>& c, uint32_t op) { c.push_back(0xfd); uleb(c, op); }
static void loadr(std::vector<uint8_t>& c, uint32_t r) { get(c,0); i32c(c, static_cast<int32_t>(r*8)); c.push_back(0x6a); i64load(c); }
static void storer(std::vector<uint8_t>& c, uint32_t r) { get(c,0); i32c(c, static_cast<int32_t>(r*8)); c.push_back(0x6a); i64store(c); }
static void loadfp(std::vector<uint8_t>& c, uint32_t p, uint32_t r) { get(c,p); i32c(c, static_cast<int32_t>(r*16)); c.push_back(0x6a); vload(c); }
static void storefp(std::vector<uint8_t>& c, uint32_t p, uint32_t r) { get(c,p); i32c(c, static_cast<int32_t>(r*16)); c.push_back(0x6a); vstore(c); }
static void address(std::vector<uint8_t>& c, uint32_t src, int64_t imm, uint32_t mask, bool zero) {
    get(c,4);
    if (zero) i64c(c, imm);
    else { get(c,5+src); i64c(c,imm); c.push_back(0x7c); }
    i64c(c,static_cast<int64_t>(mask)); c.push_back(0x83); c.push_back(0xa7); c.push_back(0x6a);
}
static void loadm(std::vector<uint8_t>& c, uint32_t src, int64_t imm, uint32_t mask, bool zero) {
    address(c,src,imm,mask,zero); i64load(c);
}
static int reg(uint8_t r) { return static_cast<int>(r % RegistersCount); }
static int flt(uint8_t r) { return static_cast<int>(r % RegisterCountFlt); }
static void mulh(std::vector<uint8_t>& c, uint32_t fn) { c.push_back(0x10); uleb(c,fn); }

static bool supported(const Instruction& ins) {
    switch (ins.opcode) {
        case IADD_RS: case IADD_M: case ISUB_R: case ISUB_M:
        case IMUL_R: case IMUL_M: case IMULH_R: case IMULH_M:
        case ISMULH_R: case ISMULH_M: case IMUL_RCP: case INEG_R:
        case IXOR_R: case IXOR_M: case IROR_R: case IROL_R:
        case ISWAP_R: case FSWAP_R: case FADD_R: case FSUB_M:
        case FMUL_R: case FDIV_M: case FSQRT_R: case CBRANCH:
        case ISTORE: case NOP:
            return true;
        default:
            return false;
    }
}

static uint64_t cbranch_imm(const Instruction& ins) {
    const uint32_t shift = static_cast<uint32_t>(ins.getModCond() + ConditionOffset);
    uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(ins.getImm32())));
    imm |= (uint64_t(1) << shift);
    if (ConditionOffset > 0 || shift > 0) imm &= ~(uint64_t(1) << (shift - 1));
    return imm;
}
static uint64_t cbranch_mask(const Instruction& ins) {
    const uint32_t shift = static_cast<uint32_t>(ins.getModCond() + ConditionOffset);
    return uint64_t(ConditionMask) << shift;
}

static void emit_instruction(std::vector<uint8_t>& c, const Instruction& ins, uint32_t pc,
                             const std::vector<int>& branchTarget) {
    const int op=ins.opcode, dst=reg(ins.dst), src=reg(ins.src);
    const int fdst=flt(ins.dst), fsrc=flt(ins.src);
    const int64_t imm=static_cast<int64_t>(static_cast<int32_t>(ins.getImm32()));
    if (op < ceil_IADD_RS) {
        get(c,5+dst); get(c,5+src); i64c(c,static_cast<int64_t>(ins.getModShift())); c.push_back(0x86);
        if (dst==RegisterNeedsDisplacement) { i64c(c,imm); c.push_back(0x7c); }
        c.push_back(0x7c); set(c,5+dst);
    } else if (op < ceil_IADD_M) {
        get(c,5+dst); bool z=src==dst; uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z); c.push_back(0x7c); set(c,5+dst);
    } else if (op < ceil_ISUB_R) {
        get(c,5+dst); if(src==dst)i64c(c,imm);else get(c,5+src); c.push_back(0x7d); set(c,5+dst);
    } else if (op < ceil_ISUB_M) {
        get(c,5+dst); bool z=src==dst; uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z); c.push_back(0x7d); set(c,5+dst);
    } else if (op < ceil_IMUL_R) {
        get(c,5+dst); if(src==dst)i64c(c,imm);else get(c,5+src); c.push_back(0x7e); set(c,5+dst);
    } else if (op < ceil_IMUL_M) {
        get(c,5+dst); bool z=src==dst; uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z); c.push_back(0x7e); set(c,5+dst);
    } else if (op < ceil_IMULH_R) {
        get(c,5+dst); get(c,5+src); mulh(c,0); set(c,5+dst);
    } else if (op < ceil_IMULH_M) {
        get(c,5+dst); bool z=src==dst; uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z); mulh(c,0); set(c,5+dst);
    } else if (op < ceil_ISMULH_R) {
        get(c,5+dst); get(c,5+src); mulh(c,1); set(c,5+dst);
    } else if (op < ceil_ISMULH_M) {
        get(c,5+dst); bool z=src==dst; uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z); mulh(c,1); set(c,5+dst);
    } else if (op < ceil_IMUL_RCP) {
        uint32_t d=ins.getImm32();
        if(!isZeroOrPowerOf2(d)){get(c,5+dst);i64c(c,static_cast<int64_t>(randomx_reciprocal(d)));c.push_back(0x7e);set(c,5+dst);}
    } else if (op < ceil_INEG_R) {
        get(c,5+dst);i64c(c,0);c.push_back(0x7d);set(c,5+dst);
    } else if (op < ceil_IXOR_R) {
        get(c,5+dst);if(src==dst)i64c(c,imm);else get(c,5+src);c.push_back(0x85);set(c,5+dst);
    } else if (op < ceil_IXOR_M) {
        get(c,5+dst);bool z=src==dst;uint32_t m=z?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        loadm(c,src,imm,m,z);c.push_back(0x85);set(c,5+dst);
    } else if (op < ceil_IROR_R) {
        get(c,5+dst);if(src==dst)i64c(c,static_cast<int64_t>(ins.getImm32()));else get(c,5+src);c.push_back(0x88);set(c,5+dst);
    } else if (op < ceil_IROL_R) {
        get(c,5+dst);if(src==dst)i64c(c,static_cast<int64_t>(ins.getImm32()));else get(c,5+src);c.push_back(0x89);set(c,5+dst);
    } else if (op < ceil_ISWAP_R) {
        if(src!=dst){get(c,5+dst);get(c,5+src);set(c,5+dst);set(c,5+src);}
    } else if (op < ceil_FSWAP_R) {
        int t=(ins.dst%RegistersCount)<RegisterCountFlt?13+fdst:17+fdst;
        get(c,t);get(c,t);vop(c,0x0d);const uint8_t sh[16]={8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7};c.insert(c.end(),sh,sh+16);set(c,t);
    } else if (op < ceil_FADD_R) {
        get(c,13+fdst);get(c,21+fsrc);vop(c,0xf0);set(c,13+fdst);
    } else if (op < ceil_FSUB_R) {
        // FADD_M is intentionally unsupported in V1.
    } else if (op < ceil_FSUB_M) {
        // FSUB_R is intentionally unsupported in V1.
    } else if (op < ceil_FSCAL_R) {
        get(c,13+fdst); get(c,21+fsrc); vop(c,0xf1); set(c,13+fdst);
    } else if (op < ceil_FMUL_R) {
        // FSCAL_R is intentionally unsupported in V1.
    } else if (op < ceil_FDIV_M) {
        get(c,13+fdst);vop(c,0x0c);const uint64_t mask=0x80F0000000000000ULL;
        for(int lane=0;lane<2;++lane)for(int b=0;b<8;++b)c.push_back(static_cast<uint8_t>(mask>>(8*b)));
        vop(c,0x51);set(c,13+fdst);
    } else if (op < ceil_FSQRT_R) {
        get(c,17+fdst);get(c,21+fsrc);vop(c,0xf2);set(c,17+fdst);
    } else if (op < ceil_CBRANCH) {
        get(c,17+fdst);vop(c,0xef);set(c,17+fdst);
    } else if (op < ceil_ISTORE) {
        // CFROUND is intentionally unsupported in V1.
    } else if (op < ceil_NOP) {
        uint32_t m=(ins.getModCond()>=StoreL3Condition)?ScratchpadL3Mask:(ins.getModMem()?ScratchpadL1Mask:ScratchpadL2Mask);
        address(c,dst,imm,m,false);get(c,5+src);i64store(c);
    }

    if (op == ceil_CBRANCH - 1) {
        const uint64_t ci=cbranch_imm(ins), cm=cbranch_mask(ins);
        get(c,5+dst); i64c(c,static_cast<int64_t>(ci)); c.push_back(0x7c); set(c,5+dst);
        get(c,5+dst); i64c(c,static_cast<int64_t>(cm)); c.push_back(0x83); i64c(c,0); c.push_back(0x51);
        c.push_back(0x04); c.push_back(0x40);
        i32c(c,branchTarget[pc]); set(c,25);
        c.push_back(0x05); i32c(c,static_cast<int32_t>(pc+1)); set(c,25);
        c.push_back(0x0b);
    }
}

} // namespace

bool WasmJit::compile(const Program& program) {
    module_.clear();
    const uint32_t n=program.getSize();
    if(n==0 || n>256) return false;

    std::vector<int> target(n,-1), last(RegistersCount,-1);
    for(uint32_t pc=0;pc<n;++pc){
        const Instruction& ins=program(static_cast<int>(pc));
        if(!supported(ins)){
            std::cout<<"[WASM-JIT] UNSUPPORTED pc="<<pc
                     <<" opcode="<<static_cast<int>(ins.opcode)<<std::endl;
            return false;
        }
        const int op=ins.opcode, d=reg(ins.dst), s=reg(ins.src);
        if(op==ceil_CBRANCH-1) target[pc]=(last[d]<0)?0:last[d]+1;
        if(op < ceil_ISWAP_R){
            if(op != IMUL_RCP || !isZeroOrPowerOf2(ins.getImm32())) last[d]=static_cast<int>(pc);
        } else if(op == ceil_ISWAP_R - 1) {
            if(d!=s){last[d]=static_cast<int>(pc);last[s]=static_cast<int>(pc);}
        }
        if(op==ceil_CBRANCH-1) for(int r=0;r<RegistersCount;++r) last[r]=static_cast<int>(pc);
    }

    std::vector<uint8_t> code;
    for(int r=0;r<RegistersCount;++r){loadr(code,r);set(code,5+r);}
    for(int r=0;r<RegisterCountFlt;++r){loadfp(code,1,r);set(code,13+r);loadfp(code,2,r);set(code,17+r);loadfp(code,3,r);set(code,21+r);}
    i32c(code,0);set(code,25);

    // Structured dispatcher: case 0 is innermost, case n-1 outermost.
    code.push_back(0x02); code.push_back(0x40); // block $exit
    code.push_back(0x03); code.push_back(0x40); // loop $dispatch
    for(uint32_t k=n;k>0;--k){code.push_back(0x02);code.push_back(0x40);}
    get(code,25);
    code.push_back(0x0e); uleb(code,n);
    for(uint32_t k=0;k<n;++k) uleb(code,k);
    uleb(code,n+1); // default -> $exit

    for(uint32_t pc=0;pc<n;++pc){
        code.push_back(0x0b); // end case pc
        emit_instruction(code,program(static_cast<int>(pc)),pc,target);
        i32c(code,pc+1<n?static_cast<int32_t>(pc+1):static_cast<int32_t>(n)); set(code,25);
        code.push_back(0x0c); uleb(code,n-1-pc); // br $dispatch
    }
    code.push_back(0x0b); // end loop
    code.push_back(0x0b); // end exit

    for(int r=0;r<RegistersCount;++r){get(code,5+r);storer(code,r);}
    for(int r=0;r<RegisterCountFlt;++r){get(code,13+r);storefp(code,1,r);get(code,17+r);storefp(code,2,r);get(code,21+r);storefp(code,3,r);}
    code.push_back(0x0b);

    module_.insert(module_.end(),{0,0x61,0x73,0x6d,1,0,0,0});
    std::vector<uint8_t> type; uleb(type,3);
    type.push_back(0x60); uleb(type,5); for(int i=0;i<5;++i)type.push_back(0x7f); uleb(type,0);
    for(int t=0;t<2;++t){type.push_back(0x60);uleb(type,2);type.push_back(0x7e);type.push_back(0x7e);uleb(type,1);type.push_back(0x7e);}
    section(module_,1,type);
    std::vector<uint8_t> imports; uleb(imports,3);
    bytes(imports,"env",3);bytes(imports,"mulh_u64",8);imports.push_back(0);uleb(imports,1);
    bytes(imports,"env",3);bytes(imports,"mulh_s64",8);imports.push_back(0);uleb(imports,2);
    bytes(imports,"env",3);bytes(imports,"memory",6);imports.push_back(2);imports.push_back(0);uleb(imports,1);
    section(module_,2,imports);
    std::vector<uint8_t> funcs;uleb(funcs,1);uleb(funcs,0);section(module_,3,funcs);
    std::vector<uint8_t> exports;uleb(exports,1);bytes(exports,"rx_jit",6);exports.push_back(0);uleb(exports,2);section(module_,7,exports);
    std::vector<uint8_t> body;uleb(body,3);uleb(body,8);body.push_back(0x7e);uleb(body,12);body.push_back(0x7b);uleb(body,1);body.push_back(0x7f);body.insert(body.end(),code.begin(),code.end());
    std::vector<uint8_t> codes;uleb(codes,1);uleb(codes,static_cast<uint32_t>(body.size()));codes.insert(codes.end(),body.begin(),body.end());section(module_,10,codes);
    return true;
}

} // namespace randomx

#endif
