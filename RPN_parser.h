#pragma once

#include <llvm.h>
#include <math.h>

#include <sstream>

using namespace std;

// runtime func support
extern "C" {
float wave(float amp, float hz, float phase) { return amp * sinf(hz + phase); }
float factorial(float f) {
  if (f <= 0)
    return 1;
  else
    return f * factorial(f - 1);
}
}

class JIT {
 public:
  LLVMContext Context;
  IRBuilder<> Builder = IRBuilder<>(Context);
  unique_ptr<Module> module;
  ExecutionEngine *engine = nullptr;
  EngineBuilder *engine_builder = nullptr;
  string error_msg, code;

 public:
  JIT(string module_name = "test") {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    module = make_unique<Module>(module_name, Context);
  }
  ~JIT() {
    if (engine) delete engine;
    if (engine_builder) delete engine_builder;
  }

  bool create_engine() {  //  create execution engine
    engine_builder = new EngineBuilder(move(module));
    engine_builder->setErrorStr(&error_msg).setEngineKind(EngineKind::JIT);
    engine = engine_builder->create();

    return engine != nullptr;
  }

  void dump_code() {  // Output the bitcode file to stdout
    WriteBitcodeToFile(*module.get(), outs());
  }

  void show_code() {
    module->print(outs(), nullptr);  // print IR
  }

  string get_ir_code() {
    string str;

    raw_string_ostream os(str);

    os << *module;
    os.flush();

    return str;
  }

  Module *get_module() { return module.get(); }

  intptr_t get_function(string name) {
    return engine->getFunctionAddress(name);
  }

  vector<Type *> arg_types(vector<Value *> args) {
    vector<Type *> arg_tys;
    for (Value *v : args) arg_tys.push_back(v->getType());
    return arg_tys;
  }

  vector<Type *> arg_fp(int n) {
    vector<Type *> vt;
    for (int i = 0; i < n; i++) vt.push_back(fp_type());
    return vt;
  }

  vector<Type *> arg_dbl(int n) {
    vector<Type *> vt;
    for (int i = 0; i < n; i++) vt.push_back(dbl_type());
    return vt;
  }

  int64_t get_int(Value *v) { return dyn_cast<ConstantInt>(v)->getSExtValue(); }
  double get_dbl(Value *v) {
    return dyn_cast<ConstantFP>(v)->getValueAPF().convertToDouble();
  }
  double get_flt(Value *v) {
    return dyn_cast<ConstantFP>(v)->getValueAPF().convertToFloat();
  }

  Type *int32_type() { return Type::getInt32Ty(Context); }
  Type *fp_type() { return Type::getFloatTy(Context); }
  Type *dbl_type() { return Type::getDoubleTy(Context); }
  Type *void_type() { return Type::getVoidTy(Context); }

  Value *init_fp(string name = "", float f = 0) {
    auto v = ConstantFP::get(Type::getFloatTy(Context), f);
    v->setName(name);
    return v;
  }
  Value *init_dbl(string name = "", double d = 0) {
    auto v = ConstantFP::get(Type::getDoubleTy(Context), d);
    v->setName(name);
    return v;
  }
};

class RPN_parser : public JIT {
 public:
  enum {  // list of symbols
    SNULL = 0,
    NUMBER,
    IDENT,
    IDENT_x,
    IDENT_t,
    PLUS,
    MINUS,
    MULT,
    DIV,
    OPAREN,
    CPAREN,
    FACT,
    POWER,
    PERIOD,
    EQ,
    GT,
    GE,
    LT,
    LE,
    NE,
    QUESTION,
    COLON
  };

  // function names
  enum {  // symbol start from 50
    FSIN = 90,
    FCOS,
    FTAN,
    FEXP,
    FLOG,
    FLOG10,
    FFLOOR,
    FSQRT,
    FASIN,
    FACOS,
    FATAN,
    FABS,

    FWAVE,

    SPI,
    SPHI,
    S_E
  };

  const vector<string> fname = {
      "sin",  "cos",  "tan",  "exp",  "log",  "log10", "floor", "sqrt",
      "asin", "acos", "atan", "fabs", "wave", "pi",    "phi",   "e"};

  string s;
  int ix_s = 0;
  char ch = 0;

  int sym = SNULL;
  string id;
  float nval = 0;  // actual numerical value
  bool err = false;

  using func_type = float(float);  // y=rnp_func(x)
  func_type *func = nullptr;

  map<int, Function *> funcs;

  class Stack {
   public:
    vector<Value *> stack;

    Value *get(int sp_offset = 0) {  // get item _stack[-sp_offset]
      return sp_offset == 0 ? stack.back()
                            : *::prev(stack.end(), sp_offset + 1);
    }

    Value *top() { return stack.back(); }
    Value *prev() { return get(1); }  // stack[sp-1]

    void push(Value *v) { stack.push_back(v); }
    void pop(int n_times = 1) {
      for (int i = 0; i < n_times; i++) stack.pop_back();
    }

    Instruction *bin_oper(Instruction *instr) {
      pop(2);
      push(instr);
      return instr;
    }

    Value *bin_oper(Value *v) {
      pop(2);
      push(v);
      return v;
    }

    Instruction *tri_oper(Instruction *instr) {
      pop(3);
      push(instr);
      return instr;
    }

    Instruction *unit_oper(Instruction *instr) {
      pop();
      push(instr);
      return instr;
    }

    int size() { return stack.size(); }

    void clear() { stack.clear(); }
  };

  Stack stack;

  RPN_parser(string s) : JIT("rpn_main"), s(s + " ") {
    // float rpn_result = float rpn_func(float x)
    Function *main_function =
        Function::Create(FunctionType::get(fp_type(), {fp_type()}, false),
                         Function::ExternalLinkage, "rpn_func", get_module());
    Builder.SetInsertPoint(
        BasicBlock::Create(Context, "EntryBlock_rpn", main_function));

    Value *x_value = main_function->arg_begin(), *zero_v = init_fp("zero", 0);

    stack.clear();

    getch();

    static map<int, float> constants_map{
        {SPHI, 1.61803f}, {SPI, M_PI}, {S_E, M_E}};

    while (getsym() != SNULL && !err) {
      switch (sym) {
        case SPI:
        case SPHI:
        case S_E:
          nval = constants_map[sym];
        case NUMBER:
          stack.push(init_fp("constant", nval));
          break;
        case IDENT_x:
        case IDENT_t:
          stack.push(x_value);
          break;

        case PLUS:
          if (stack.size() >= 2)
            stack.bin_oper(
                Builder.CreateFAdd(stack.top(), stack.prev(), "tmp"));
          else
            err = true;
          break;

        case MINUS:
          if (stack.size() >= 2)
            stack.bin_oper(
                Builder.CreateFSub(stack.prev(), stack.top(), "tmp"));
          else
            err = true;
          break;

        case MULT:
          if (stack.size() >= 2)
            stack.bin_oper(
                Builder.CreateFMul(stack.top(), stack.prev(), "tmp"));
          else
            err = true;
          break;
        case DIV:
          if (stack.size() >= 2)
            stack.bin_oper(
                Builder.CreateFDiv(stack.prev(), stack.top(), "tmp"));
          else
            err = true;
          break;

          // 2 arg func

        case POWER:
          if (stack.size() >= 2) {
            stack.bin_oper(Builder.CreateCall(
                create_func(), {stack.prev(), stack.top()}, "tmp"));
          } else
            err = true;
          break;

        case FWAVE:
          if (stack.size() >= 3) {  // amp->a2 * sinf(hz->a1 + phase->a0)
            stack.tri_oper(Builder.CreateCall(
                create_func(), {stack.get(2), stack.get(1), stack.top()},
                "tmp"));
          } else
            err = true;
          break;

        case QUESTION:  // x(2) a(1) b(top) ?
          if (stack.size() >= 3) {
            Function *func = Builder.GetInsertBlock()->getParent();

            // Create blocks for the then and else cases.  Insert the 'then'
            // block at the end of the function.
            BasicBlock *ThenBB = BasicBlock::Create(Context, "then", func),
                       *ElseBB = BasicBlock::Create(Context, "else"),
                       *MergeBB = BasicBlock::Create(Context, "ifcont");

            Builder.CreateCondBr(
                Builder.CreateFCmpONE(
                    init_fp("zero", 0),  // Convert condition to a bool by
                                         // comparing non-equal to 0.0.
                    stack.get(2), "ifcond"),
                ThenBB, ElseBB);

            // Emit then value.
            Builder.SetInsertPoint(ThenBB);

            auto ThenV = BinaryOperator::Create(Instruction::FAdd, stack.get(1),
                                                zero_v, "tmp_then");
            ThenBB->getInstList().push_back(ThenV);

            Builder.CreateBr(MergeBB);
            // Codegen of 'Then' can change the current block, update ThenBB for
            // the PHI.
            ThenBB = Builder.GetInsertBlock();

            // Emit else block.
            func->getBasicBlockList().push_back(ElseBB);
            Builder.SetInsertPoint(ElseBB);

            auto *ElseV = BinaryOperator::Create(Instruction::FAdd, stack.top(),
                                                 zero_v, "tmp_else");

            ElseBB->getInstList().push_back(ElseV);

            Builder.CreateBr(MergeBB);
            // Codegen of 'Else' can change the current block, update ElseBB for
            // the PHI.
            ElseBB = Builder.GetInsertBlock();

            // Emit merge block.
            func->getBasicBlockList().push_back(MergeBB);
            Builder.SetInsertPoint(MergeBB);
            PHINode *PN =
                Builder.CreatePHI(Type::getFloatTy(Context), 2, "iftmp");

            PN->addIncoming(ThenV, ThenBB);
            PN->addIncoming(ElseV, ElseBB);

            stack.tri_oper(PN);

          } else
            err = true;
          break;

        case EQ:
        case NE:
        case GT:
        case LT:
        case LE:
        case GE: {
          static map<int, CmpInst::Predicate> op_map{
              {EQ, CmpInst::FCMP_OEQ}, {NE, CmpInst::FCMP_ONE},
              {GT, CmpInst::FCMP_OGT}, {LT, CmpInst::FCMP_OLT},
              {LE, CmpInst::FCMP_OLE}, {GE, CmpInst::FCMP_OGE}};

          if (stack.size() >= 2) {
            stack.bin_oper(Builder.CreateUIToFP(
                Builder.CreateFCmp(op_map[sym], stack.prev(), stack.top(),
                                   "tmp"),
                fp_type()));

          } else
            err = true;
        } break;

        case FACT:
          if (stack.size() >= 1) {
            stack.unit_oper(
                Builder.CreateCall(create_func(), {stack.top()}, "tmp"));
          } else
            err = true;
          break;

        default:
          if (stack.size() >= 1 && sym >= FSIN && sym <= FABS) {  // 1 arg func
            stack.unit_oper(
                Builder.CreateCall(create_func(), {stack.top()}, "tmp"));
          } else
            err = true;
          break;
      }
    }

    if (stack.size() == 1 && !err) {
      Builder.CreateRet(stack.top());

      code = get_ir_code();

      if (!create_engine()) {
        err = true;
      } else {  // set the 'c' callable func that evaluates the rpn expression
        func =
            reinterpret_cast<func_type *>(((void *)get_function("rpn_func")));
      }
    } else
      err = true;

    stack.clear();
  }

  Function *new_fp_func(string name, int n_params) {
    Function *fnc =
        Function::Create(FunctionType::get(fp_type(), arg_fp(n_params), false),
                         Function::ExternalLinkage, name, get_module());
    fnc->setCallingConv(CallingConv::C);
    return fnc;
  }

  Function *create_func() {  // create internal func on demand by 'sym'

    switch (sym) {
      case POWER:
        funcs[sym] = new_fp_func("powf", 2);
        break;
      case FWAVE:
        funcs[sym] = new_fp_func("wave", 3);
        break;
      case FACT:
        funcs[sym] = new_fp_func("factorial", 1);
        break;
      default:
        if (funcs.find(sym) == funcs.end())
          funcs[sym] = new_fp_func(fname[sym - FSIN] + "f", 1);
        break;
    }

    return funcs[sym];
  }

  inline float evaluate(float x) { return func(x); }

  bool ok() { return !err; }

  char getch(void) { return ch = (ix_s < int(s.length())) ? s[ix_s++] : 0; }
  void ungetch(void) {
    if (ix_s > 0) ix_s--;
  }

  int getsym(void) {
    sym = SNULL;
    id.clear();

    // skip blanks
    while (ch && ch <= ' ') getch();

    // detect symbol
    if (isalpha(ch)) {  // ident
      while (isalnum(ch) || ch == '_') {
        id.push_back(tolower(ch));
        getch();
      }
      sym = IDENT;

      if (id == "x")
        sym = IDENT_x;
      else if (id == "t")
        sym = IDENT_t;
      else {
        // is a funct ?
        auto fnd = find(fname.begin(), fname.end(), id);
        if (fnd != fname.end())
          sym = distance(fname.begin(), fnd) + FSIN;  // first symbol offset
      }
    } else {
      if (isdigit(ch)) {  // number (float) take care of dddd.ddde-dd
        while (isdigit(ch) || ch == '.' || ch == 'e' || ch == 'E') {
          id.push_back(ch);
          getch();
        }
        sym = NUMBER;
        nval = stof(id);
      } else {
        switch (ch) {
          case '+':
            sym = PLUS;
            break;
          case '-':
            sym = MINUS;
            break;
          case '*':
            sym = MULT;
            break;
          case '/':
            sym = DIV;
            break;
          case '(':
            sym = OPAREN;
            break;
          case ')':
            sym = CPAREN;
            break;
          case '!':
            sym = FACT;
            break;
          case '^':
            sym = POWER;
            break;
          case ',':
            sym = PERIOD;
            break;

          case '=':
            sym = EQ;
            break;

          case '>':
            getch();
            if (ch == '=')
              sym = GE;
            else {
              ungetch();
              sym = GT;
            }
            break;

          case '<':
            getch();
            switch (ch) {
              case '>':
                sym = NE;
                break;
              case '=':
                sym = LE;
                break;
              default:
                ungetch();
                sym = LT;
                break;
            }
            break;

          case '?':
            sym = QUESTION;
            break;
          case ':':
            sym = COLON;
            break;

          case 0:
            sym = SNULL;
            break;

          default:
            sym = SNULL;
            err = true;
            break;
        }
        getch();
      }
    }

    return sym;
  }
};
