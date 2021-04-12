/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file relay/backend/graph_codegen.cc
 * \brief Graph runtime codegen
 */

#include <dmlc/any.h>
#include <tvm/ir/module.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/runtime/device_api.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/expr.h>
#include <tvm/tir/stmt.h>

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include "../../runtime/meta_data.h"
#include "compile_engine.h"
#include "utils.h"

namespace tvm {
namespace relay {
namespace backend {

using IntegerArray = Array<Integer>;
using ShapeVector = std::vector<std::vector<int64_t>>;
using GraphAttrs = std::unordered_map<std::string, dmlc::any>;
using TargetsMap = std::unordered_map<int, Target>;

/*! \brief Lowered outputs */
struct AOTLoweredOutput {
  tir::PrimFunc runner_func;
  Map<String, IRModule> lowered_funcs;
  Array<tvm::runtime::Module> external_mods;
  std::unordered_map<std::string, std::pair<int, const tvm::runtime::NDArray>> params;
  runtime::AOTMetadata aot_metadata;
};

class AotReturnSidVisitor : public ExprVisitor {
 public:
  explicit AotReturnSidVisitor(Map<Expr, Array<IntegerArray>> storage_device_map)
      : storage_device_map_{storage_device_map}, return_sid_{-1} {}

  IntegerArray FindReturnSid(Function func) {
    VisitExpr(func->body);
    return return_sid_;
  }

 protected:
  void AssignReturnSid(Expr e) {
    auto iter = storage_device_map_.find(e);
    if (iter != storage_device_map_.end()) {
      return_sid_ = (*iter).second[0];
    }
  }

  void VisitExpr_(const ConstantNode* cn) override {
    ExprVisitor::VisitExpr_(cn);
    AssignReturnSid(GetRef<Expr>(cn));
  }

  void VisitExpr_(const VarNode* vn) override {
    ExprVisitor::VisitExpr_(vn);
    AssignReturnSid(GetRef<Expr>(vn));
  }

  void VisitExpr_(const CallNode* cn) override {
    ExprVisitor::VisitExpr_(cn);
    AssignReturnSid(GetRef<Expr>(cn));
  }

  void VisitExpr_(const LetNode* op) override { VisitExpr(op->body); }

  void VisitExpr_(const TupleNode* tn) override {
    ExprVisitor::VisitExpr_(tn);
    AssignReturnSid(GetRef<Expr>(tn));
  }

 private:
  Map<Expr, Array<IntegerArray>> storage_device_map_;
  IntegerArray return_sid_;
};

/*! \brief Code generator for AOT executor */
class AOTCodegen : public ExprVisitor {
 protected:
  /*!
   * \brief Utility function to allocate a DLTensor or TVMValue
   * \param  type the type of allocation
   * \param num the number of variable to allocate on the stack
   * \return PrimExpr representing the allocated object
   */
  PrimExpr StackAlloca(std::string type, size_t num) {
    Array<PrimExpr> args = {tir::StringImm(type), ConstInt32(num)};
    return tir::Call(DataType::Handle(), tir::builtin::tvm_stack_alloca(), args);
  }

  /*!
   * \brief Utility function to allocate memory for storage identifiers
   * \param  memory_size_byte size in bytes of the allocation
   * \return PrimExpr representing the allocated memory
   */
  PrimExpr AllocateBackendMemory(int memory_size_byte) {
    // TODO(giuseros): use tir::Allocate instead of TVMBackendAllocWorkspace
    // to enable unified memory planning
    static const Op& op = Op::Get("tir.TVMBackendAllocWorkspace");
    return tvm::tir::Call(DataType::Handle(), op, {1, 0, memory_size_byte, 2, 8});
  }

  /*!
   * \brief Utility function to convert a concrete integer to a PrimExpr.
   * \param num the number to convert
   * \return PrimExpr representing num
   */
  inline PrimExpr ConstInt32(size_t num) {
    ICHECK_LE(num, std::numeric_limits<int>::max());
    return tir::make_const(DataType::Int(32), static_cast<int>(num));
  }

  /*!
   * \brief Return a vector of variables that represents the sids for the given Relay Expr
   */
  std::vector<tir::Var> pack_sid(Expr expr) {
    Array<IntegerArray> sids = storage_device_map_[expr];
    std::vector<tir::Var> sid_vars;

    // Note that an expression can have multiple sids associated with it
    // e.g., returning multiple values from a function
    for (const auto& sid : sids[0]) {
      // Determine if an sid is an output buffer
      int sid_int = static_cast<int>((sid.as<IntImmNode>())->value);
      auto output_iter = std::find(return_sid_.begin(), return_sid_.end(), sid_int);
      if (output_iter != return_sid_.end()) {
        int output_index = std::distance(return_sid_.begin(), output_iter);
        sid_vars.push_back(main_signature_[input_vars_.size() + output_index]);
        continue;
      }
      // Pack the sid inside the TVMValue
      auto sid_array = te::Var(make_string("sid_", sid, "_value"), DataType::Handle());
      auto sid_value = sids_table_[sid];
      tvm::PrimExpr set_tensor =
          tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_set(),
                         {sid_array, 0, tir::builtin::kArrData, sid_value});
      stmts_.push_back(tir::LetStmt(sid_array, StackAlloca("array", 1), tir::Evaluate(set_tensor)));
      sid_vars.push_back(sid_array);
    }
    return sid_vars;
  }

  /*!
   * \brief Utility function to return a parameter associated with an expression
   * \param expr Relay Expression assicated with the parameter
   * \return Variable that represents the DLTensor associated with the parameters
   */
  tir::Var pack_param(Expr expr) {
    // TODO(giuseros): Using call_extern to call into lookup_linked_param. This is because the
    // builtin::ret is not supported yet in the c target. Once return is supported we can use
    // tvm_call_packed_lowered().
    int param_sid = param_storage_ids_[params_by_expr_[expr]];
    auto lookup_linked_param_fn = tir::StringImm(::tvm::runtime::symbol::tvm_lookup_linked_param);
    auto param_array = te::Var(make_string("param_", param_sid, "_array"), DataType::Handle());

    // Compose the lookup_call using a local stack
    Array<tir::Stmt> lookup_call;
    auto param_var = te::Var(make_string("param_", param_sid, "_value"), DataType::Handle());
    auto ret_var = te::Var("ret_value", DataType::Handle());
    auto ret_code = te::Var("ret_value", DataType::Handle());

    lookup_call.push_back(tir::Evaluate(
        tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_set(),
                       {param_var, 0, tir::builtin::kTVMValueContent, ConstInt32(param_sid)})));
    lookup_call.push_back(tir::Evaluate(
        tvm::tir::Call(DataType::Handle(), tir::builtin::call_extern(),
                       {lookup_linked_param_fn, param_var, 0, 0, ret_var, ret_code, 0})));
    auto ret_var_handle = tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_get(),
                                         {ret_var, 0, tir::builtin::kTVMValueContent});

    // Set the param to the value returned by lookup_call
    tvm::PrimExpr set_param_array =
        tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_set(),
                       {param_array, 0, tir::builtin::kArrData, ret_var_handle});
    lookup_call.push_back(tir::Evaluate(set_param_array));

    tir::Stmt lookup_body = tir::SeqStmt(lookup_call);

    // Allocate the DLTensors on the stack
    lookup_body = tir::LetStmt(param_var, StackAlloca("arg_value", 1), lookup_body);
    lookup_body = tir::LetStmt(ret_var, StackAlloca("arg_value", 1), lookup_body);
    lookup_body = tir::LetStmt(ret_code, StackAlloca("arg_value", 1), lookup_body);
    lookup_body = tir::LetStmt(param_array, StackAlloca("arg_value", 1), lookup_body);
    stmts_.push_back(lookup_body);
    return param_array;
  }

  /*!
   * brief Given an expression return the variable(s) associated with that expression
   */
  std::vector<te::Var> find_expr(Expr arg) {
    auto input_iter = std::find(input_vars_.begin(), input_vars_.end(), arg);
    if (input_iter != input_vars_.end()) {
      // Input variable
      int main_index = std::distance(input_vars_.begin(), input_iter);
      return {main_signature_[main_index]};
    } else if (params_by_expr_.find(arg) != params_by_expr_.end()) {
      // Parameter of the network
      return {pack_param(arg)};
    } else {
      // Storage identifier (i.e., intermediate memory)
      return pack_sid(arg);
    }
  }

  /*!
   * brief Call a function with a given name
   */
  void func_call(Call call, std::string func_name) {
    tvm::Array<PrimExpr> args{tvm::tir::StringImm(func_name)};
    std::vector<tir::Stmt> func_call_stmts;

    // Pack the inputs
    for (Expr arg : call->args) {
      auto var_arg = find_expr(arg);
      args.push_back(var_arg[0]);
    }

    auto ret_expr = Downcast<Expr>(call);

    // Pack the return(s) value. A call node can produce multiple outputs
    for (const auto& var : pack_sid(ret_expr)) {
      args.push_back(var);
    }

    // Use tvm_call_packed to execute the function
    func_call_stmts.push_back(tir::Evaluate(
        tvm::tir::Call(DataType::Int(32), tvm::tir::builtin::tvm_call_packed(), args)));
    tir::Stmt body = tir::SeqStmt(func_call_stmts);
    stmts_.push_back(body);
  }

  /*!
   * brief Copy a variable to the output. This function is mainly used in edge cases
   * when we want to return an input or a parameter.
   */
  void copy_to_output(te::Var out, te::Var in, size_t size) {
    auto retval_get = tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_get(),
                                     {in, 0, tir::builtin::kArrData});

    // Define intermediate DLTensor to load/store the data
    auto tmp0 = te::Var("tmp0", DataType::Handle());
    auto tmp1 = te::Var("tmp1", DataType::Handle());
    te::Var loop_idx("i", DataType::Int(32));
    auto retval_i = tir::Load(DataType::UInt(8), tmp0, loop_idx, tir::const_true());
    auto tostore = tvm::tir::Call(DataType::Handle(), tvm::tir::builtin::tvm_struct_get(),
                                  {out, 0, tir::builtin::kArrData});

    // Copy the variable from the input to the output
    tir::Stmt copy = tir::For(
        loop_idx, 0, ConstInt32(size), tir::ForKind::kSerial,
        tir::Store(tmp1, tir::Let(tmp0, retval_get, retval_i), loop_idx, tir::const_true()));
    stmts_.push_back(tir::LetStmt(tmp1, tostore, copy));
  }

  /*!
   * Utility function to string together different arguments
   */
  template <typename... Args>
  std::string make_string(Args const&... args) {
    std::ostringstream ss;
    using List = int[];
    (void)List{0, ((void)(ss << args), 0)...};

    return ss.str();
  }

  void VisitExpr_(const CallNode* op) override {
    // Descend the call tree
    for (auto arg : op->args) {
      VisitExpr(arg);
    }

    Expr expr = GetRef<Expr>(op);
    Function func;
    if (op->op.as<OpNode>()) {
      LOG(FATAL) << "Operators should be transformed away; try applying"
                 << "the fuse_ops transformation to the expression.";
    } else if (op->op.as<GlobalVarNode>()) {
      LOG(FATAL) << "Not implemented";
    } else if (op->op.as<FunctionNode>()) {
      func = GetRef<Function>(op->op.as<FunctionNode>());
    } else {
      LOG(FATAL) << "TVM runtime does not support calls to " << op->op->GetTypeKey();
    }
    if (!func->HasNonzeroAttr(attr::kPrimitive)) {
      LOG(FATAL) << "TVM only support calls to primitive functions "
                 << "(i.e functions composed of fusable operator invocations)";
    }

    auto pf0 = GetPackedFunc("relay.backend._make_CCacheKey");
    auto pf1 = GetPackedFunc("relay.backend._CompileEngineLower");
    Target target;
    // Handle external function
    if (func->GetAttr<String>(attr::kCompiler).defined()) {
      target = Target("ext_dev");
      CCacheKey key = (*pf0)(func, target);
      CachedFunc ext_func = (*pf1)(compile_engine_, key);
      ICHECK(ext_func.defined()) << "External function is not defined.";
      UpdateConstants(func, &params_);

      // Generate the TIR function call
      func_call(GetRef<Call>(op), ext_func->func_name);
    }

    ICHECK_GE(storage_device_map_.count(expr), 0);
    auto& device_type = storage_device_map_[expr][1];
    auto call_dev_type = device_type[0]->value;
    // Normal Relay Function
    if (targets_.size() == 1) {
      // homogeneous execution.
      const auto& it = targets_.begin();
      target = (*it).second;
    } else {
      // heterogeneous execution.
      std::string call_dev_name;
      if (call_dev_type == 0) {
        call_dev_name = "llvm";
      } else {
        call_dev_name = runtime::DeviceName(call_dev_type);
      }
      if (targets_.count(call_dev_type) == 0) {
        LOG(FATAL) << "No target is provided for device " << call_dev_name;
      }
      target = targets_[call_dev_type];
    }
    CCacheKey key = (*pf0)(func, target);
    CachedFunc lowered_func = (*pf1)(compile_engine_, key);
    if (!lowered_funcs_.count(target->str())) {
      lowered_funcs_[target->str()] = IRModule(Map<GlobalVar, BaseFunc>({}));
    }
    lowered_funcs_[target->str()]->Update(lowered_func->funcs);

    // Generate the TIR function call
    func_call(GetRef<Call>(op), lowered_func->func_name);
  }

  void VisitExpr_(const VarNode* op) override {
    Expr expr = GetRef<Expr>(op);

    // If the Var node is an output node we need to copy the content of the variable to the output
    // It's safe to check the SID here because Var StorageToken are never reallocated
    Array<IntegerArray> sids = storage_device_map_[expr];

    auto output_iter = std::find(return_sid_.begin(), return_sid_.end(),
                                 static_cast<int>((sids[0][0].as<IntImmNode>())->value));
    if (output_iter != return_sid_.end()) {
      int output_index = std::distance(return_sid_.begin(), output_iter);
      auto var_expr = find_expr(expr);
      copy_to_output(main_signature_[input_vars_.size() + output_index], var_expr[0], sids[2][0]);
    }
  }

  void VisitExpr_(const ConstantNode* op) override {
    Expr expr = GetRef<Expr>(op);
    size_t index = params_.size();
    std::string name = "p" + std::to_string(index);

    param_storage_ids_[name] = storage_device_map_[expr][0][0]->value;
    params_[name] = op->data;
    params_by_expr_.Set(expr, name);

    // If the Constant node is an output node we need to copy the content of the parameter to the
    // output A Var node can only produce a single output
    Array<IntegerArray> sids = storage_device_map_[expr];
    auto output_iter = std::find(return_sid_.begin(), return_sid_.end(),
                                 static_cast<int>((sids[0][0].as<IntImmNode>())->value));
    if (output_iter != return_sid_.end()) {
      int output_index = std::distance(return_sid_.begin(), output_iter);
      copy_to_output(main_signature_[input_vars_.size() + output_index], pack_param(expr),
                     sids[2][0]);
    }
  }

  void VisitExpr_(const TupleNode* op) override {
    for (auto field : op->fields) {
      VisitExpr(field);
    }
  }

  void VisitExpr_(const LetNode* op) override {
    // TODO(giuseros): support Let nodes in AOT
    CHECK(false) << "Let not yet implemented in AOT";
  }
  void VisitExpr_(const TupleGetItemNode* op) override { VisitExpr(op->tuple); }
  void VisitExpr_(const OpNode* op) override {
    throw std::runtime_error("can not compile op in non-eta expanded form");
  }
  void VisitExpr_(const GlobalVarNode* op) override { throw std::runtime_error(""); }
  void VisitExpr_(const IfNode* op) override { throw std::invalid_argument("if not supported"); }
  void VisitExpr_(const FunctionNode* op) override {
    ICHECK(op->GetAttr<String>(attr::kCompiler).defined())
        << "Only functions supported by custom codegen";
  }
  void VisitExpr_(const RefCreateNode* op) override {
    throw std::invalid_argument("reference not supported");
  }
  void VisitExpr_(const RefReadNode* op) override {
    throw std::invalid_argument("reference not supported");
  }
  void VisitExpr_(const RefWriteNode* op) override {
    throw std::invalid_argument("reference not supported");
  }
  void VisitExpr_(const ConstructorNode* op) override {
    throw std::invalid_argument("ADT constructor case not yet implemented");
  }
  void VisitExpr_(const MatchNode* op) override {
    throw std::invalid_argument("match case not yet implemented");
  }

  // Create the main PrimFunc to execute the graph
  tir::PrimFunc CreateMainFunc(unsigned int relay_params) {
    tir::Stmt body = tir::SeqStmt(stmts_);

    // Allocate the sids
    std::unordered_map<int, bool> allocated;

    for (auto kv : storage_device_map_) {
      // Only allocate sids that are needed
      const bool is_input =
          (std::find(input_vars_.begin(), input_vars_.end(), kv.first) != input_vars_.end());
      const bool is_param = (params_by_expr_.find(kv.first) != params_by_expr_.end());
      if (is_input || is_param) {
        continue;
      }

      for (unsigned int i = 0; i < kv.second[0].size(); i++) {
        int size = kv.second[2][i];
        int sid = static_cast<int>((kv.second[0][i].as<IntImmNode>())->value);

        if (std::find(return_sid_.begin(), return_sid_.end(), sid) != return_sid_.end()) {
          continue;
        }

        // TODO(giuseros): we should allocate this one time outside the PrimFunc
        // so we dont' pay the price of allocation for every inference
        if (!allocated[sid]) {
          body = tir::LetStmt(sids_table_[sid], AllocateBackendMemory(size), body);
        }
        allocated[sid] = true;
      }
    }

    // Define the attributes
    body = tir::AttrStmt(PrimExpr(), tvm::tir::attr::device_type, 1, body);
    body = tir::AttrStmt(PrimExpr(), tvm::tir::attr::device_id, 0, body);

    // Make the PrimFunc
    return tir::PrimFunc(main_signature_, body, VoidType(), Map<tir::Var, tir::Buffer>(),
                         DictAttrs(dict_attrs_));
  }

 protected:
  /*! \brief mod */
  runtime::Module* mod_;
  /*! \brief list of input expressions (i.e., variable passed by the user) */
  std::vector<Expr> input_vars_;
  /*! \brief input and output variables belonging to the main function signature */
  Array<tir::Var> main_signature_;
  /*! \brief target device */
  TargetsMap targets_;
  /*! \brief target host */
  Target target_host_;
  /*! PrimFunc attributes */
  Map<String, ObjectRef> dict_attrs_;

  /*!
   * \brief parameters (i.e. ConstantNodes found in the graph).
   * These are take as inputs to the GraphRuntime.
   * Maps param name to a pair of storage_id and NDArray. At runtime, the storage_id can be
   * used to lookup the parameter.
   */
  std::unordered_map<std::string, runtime::NDArray> params_;
  /*! \brief mapping between expression and parameters */
  Map<Expr, String> params_by_expr_;
  /*! \brief mapping between parameter names ("p0", "p1", etc..) and storage identifiers*/
  std::unordered_map<std::string, int64_t> param_storage_ids_;

  /*! \brief plan memory of device result */
  Map<Expr, Array<IntegerArray>> storage_device_map_;
  std::unordered_map<int, te::Var> sids_table_;
  /*! \brief lowered funcs */
  std::unordered_map<std::string, IRModule> lowered_funcs_;
  /*! \brief name map */
  std::unordered_map<std::string, size_t> name_map_;
  /*! \brief compile engine */
  CompileEngine compile_engine_;
  /*! \brief GraphPlanMemory module */
  runtime::Module graph_plan_memory_module_;
  /*! \brief the IR module stored which represents the executor program */
  Map<String, IRModule> tir_module_;
  /*! \brief the set of statements that make the program */
  std::vector<tir::Stmt> stmts_;
  /*! \brief the list of return sids (note that the function might return more then one output */
  IntegerArray return_sid_;

 public:
  AOTCodegen(runtime::Module* mod, const TargetsMap& targets, Target target_host)
      : mod_(mod), return_sid_() {
    compile_engine_ = CompileEngine::Global();
    targets_ = targets;
    target_host_ = target_host;
    dict_attrs_.Set("global_symbol", runtime::String("tvm__run_func"));
  }

  AOTLoweredOutput Codegen(relay::Function func) {
    // Get the module, storage map and token sizes
    auto pf = GetPackedFunc("relay.backend.GraphPlanMemory");
    storage_device_map_ = (*pf)(func);

    int input_index = 0;
    for (auto input : func->params) {
      input_vars_.push_back(input);
      main_signature_.push_back(tir::Var(make_string("input_", input_index), DataType::Handle()));
    }

    // Define the storage allocator ids
    for (auto kv : storage_device_map_) {
      for (const auto& sid : kv.second[0]) {
        te::Var sid_var(make_string("sid_", sid), DataType::Handle());
        sids_table_[sid] = sid_var;
      }
    }

    // Find the return sid
    return_sid_ = AotReturnSidVisitor(storage_device_map_).FindReturnSid(func);
    for (unsigned int output_index = 0; output_index < return_sid_.size(); output_index++) {
      main_signature_.push_back(tir::Var(make_string("output_", output_index), DataType::Handle()));
    }

    VisitExpr(func->body);

    auto prim_func = CreateMainFunc(func->params.size());
    AOTLoweredOutput ret;

    ret.params = std::unordered_map<std::string, std::pair<int, const tvm::runtime::NDArray>>();
    for (auto param : params_) {
      ret.params.emplace(std::make_pair(
          param.first,
          std::make_pair(static_cast<int>(param_storage_ids_[param.first]), param.second)));
    }

    for (auto& kv : lowered_funcs_) {
      if (ret.lowered_funcs.count(kv.first) == 0) {
        ret.lowered_funcs.Set(kv.first, IRModule(Map<GlobalVar, BaseFunc>({})));
      }
      auto& mod = ret.lowered_funcs[kv.first];
      mod->Update(kv.second);
      ret.lowered_funcs.Set(kv.first, mod);
    }
    ret.external_mods = compile_engine_->LowerExternalFunctions();

    auto target_host_str = target_host_->str();
    if (ret.lowered_funcs.find(target_host_str) != ret.lowered_funcs.end()) {
      ret.lowered_funcs[target_host_str]->Add(
          GlobalVar(::tvm::runtime::symbol::tvm_run_func_prefix), prim_func);
    } else {
      Map<GlobalVar, BaseFunc> symbol_map;
      symbol_map.Set(GlobalVar(::tvm::runtime::symbol::tvm_run_func_prefix), prim_func);
      ret.lowered_funcs.Set(target_host_str, IRModule(symbol_map));
    }

    ret.runner_func = prim_func;
    ret.aot_metadata = runtime::AOTMetadata(input_vars_.size(), return_sid_.size());
    return ret;
  }
};

class AOTCodegenModule : public runtime::ModuleNode {
 public:
  AOTCodegenModule() {}
  virtual PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) {
    if (name == "init") {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        ICHECK_EQ(args.num_args, 3) << "The expected of arguments are: "
                                    << "runtime::Module mod and Map<int, Target> targets";
        void* mod = args[0];
        Map<Integer, tvm::Target> tmp = args[1];
        tvm::Target target_host = args[2];
        init(mod, tmp, target_host);
      });
    } else if (name == "codegen") {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        Function func = args[0];
        this->output_ = codegen(func);
      });
    } else if (name == "get_runner_function") {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        *rv = get_runner_function();
      });  // c; });
    } else if (name == "list_params_name") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = list_params_name(); });
    } else if (name == "get_param_by_name") {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        String key = args[0];
        *rv = get_param_by_name(key);
      });
    } else if (name == "get_param_id") {
      return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
        String key = args[0];
        *rv = get_param_id(key);
      });
    } else if (name == "get_irmodule") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = get_irmodule(); });
    } else if (name == "get_external_modules") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = get_external_modules(); });
    } else if (name == "get_aot_metadata") {
      return PackedFunc(
          [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = get_aot_metadata(); });
    } else {
      return PackedFunc([](TVMArgs args, TVMRetValue* rv) {});
    }
  }

  const char* type_key() const final { return "RelayGraphRuntimeCodegenModule"; }

 private:
  void init(void* mod, Map<Integer, tvm::Target> tmp, Target target_host) {
    TargetsMap targets;
    for (const auto& it : tmp) {
      auto dev_type = it.first.as<tir::IntImmNode>();
      ICHECK(dev_type);
      targets[dev_type->value] = it.second;
    }
    codegen_ =
        std::make_shared<AOTCodegen>(reinterpret_cast<runtime::Module*>(mod), targets, target_host);
  }

  AOTLoweredOutput codegen(Function func) { return this->codegen_->Codegen(func); }

  tir::PrimFunc get_runner_function() { return this->output_.runner_func; }

  Array<runtime::String> list_params_name() {
    Array<runtime::String> ret;
    for (const auto& kv : this->output_.params) {
      ret.push_back(kv.first);
    }
    return ret;
  }

  runtime::NDArray get_param_by_name(String key) {
    auto it = this->output_.params.find(key);
    CHECK(it != this->output_.params.end()) << "no such parameter " << key;
    return (*it).second.second;
  }

  Array<tvm::runtime::Module> get_external_modules() { return output_.external_mods; }

  int get_param_id(String key) {
    auto it = this->output_.params.find(key);
    CHECK(it != this->output_.params.end()) << "no such parameter " << key;
    return (*it).second.first;
  }

  Map<String, IRModule> get_irmodule() { return this->output_.lowered_funcs; }

  runtime::AOTMetadata get_aot_metadata() { return output_.aot_metadata; }

  std::shared_ptr<AOTCodegen> codegen_;
  AOTLoweredOutput output_;
};

runtime::Module CreateAOTCodegenMod() {
  auto ptr = make_object<AOTCodegenModule>();
  return runtime::Module(ptr);
}

TVM_REGISTER_GLOBAL("relay.build_module._GraphAOTCodegen")
    .set_body([](TVMArgs args, TVMRetValue* rv) { *rv = CreateAOTCodegenMod(); });

}  // namespace backend
}  // namespace relay
}  // namespace tvm
