#define DEBUG_TYPE "lx"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include <iostream>

#include "Dandelion/Node.h"
#include "GraphGeneratorPass.h"

using namespace llvm;
using namespace std;
using namespace graphgen;
using namespace dandelion;

using InstructionList = std::list<InstructionNode>;
using ArgumentList = std::list<ArgumentNode>;
using BasicBlockList = std::list<SuperNode>;
using NodeList = std::list<Node>;

extern cl::opt<string> XKETCHName;
extern cl::opt<string> config_path;

namespace graphgen {

char GraphGeneratorPass::ID = 0;

RegisterPass<GraphGeneratorPass> X("graphgen", "Generating xketch graph");
}  // namespace graphgen

template <typename Iter, typename Q>
void push_range(Q &q, Iter begin, Iter end) {
    for (; begin != end; ++begin) q.push(*begin);
}

void inline findAllLoops(Loop *L, SetVector<Loop *> &Loops) {
    // Recursively find all subloops.
    for (Loop *SL : L->getSubLoops()) {
        findAllLoops(SL, Loops);
    }
    // Store current loop
    Loops.insert(L);
}

uint32_t returnNumPred(BasicBlock *BB) {
    uint32_t c = 0;
    for (auto _bb_it : llvm::predecessors(BB)) {
        if (BB != _bb_it) c++;
    }
    return c;
}

/**
 * definedInCaller - Return true if the specified value is defined in the
 * function being code extracted, but not in the region being extracted. These
 * values must be passed in as live-ins to the function.
 */
bool definedInCaller(const SetVector<BasicBlock *> &Blocks, Value *V) {
    if (isa<Argument>(V)) return true;
    if (Instruction *I = dyn_cast<Instruction>(V))
        if (!Blocks.count(I->getParent())) return true;
    return false;
}

/** definedInRegion - Return true if the specified value is defined in the
 * extracted region.
 */
bool definedInRegion(const SetVector<BasicBlock *> &Blocks, Value *V) {
    if (Instruction *I = dyn_cast<Instruction>(V))
        if (Blocks.count(I->getParent())) return true;
    return false;
}

template <class T>
Instruction *findParallelInstruction(Function &F) {
    for (auto &BB : F) {
        for (auto &I : BB) {
            if (isa<T>(I)) return &I;
        }
    }
    return nullptr;
}

template <class T>
std::vector<T *> getNodeList(Graph *_graph) {
    std::vector<T *> return_list;
    for (auto &_node : _graph->instructions()) {
        if (auto cast_node = dyn_cast<T>(&*_node))
            return_list.push_back(cast_node);
    }
    return return_list;
}

static SetVector<Loop *> getLoops(LoopInfo &LI) {
    SetVector<Loop *> Loops;

    // iterate through top level loops. Store all subloops
    // and top level loop in Loops SetVector.
    for (auto &L : LI) {
        findAllLoops(L, Loops);
    }
    return Loops;
}

std::set<Loop *> getOuterLoops(LoopInfo &LI) {
    std::set<Loop *> outer_loops;
    for (auto &L : getLoops(LI)) {
        if (L->getLoopDepth() == 1) outer_loops.insert(L);
    }
    return outer_loops;
}

void UpdateLiveInConnections(Loop *_loop, LoopNode *_loop_node,
                             std::map<llvm::Value *, Node *> map_value_node) {
    for (auto B : _loop->blocks()) {
        for (auto &I : *B) {
            // Detecting Live-ins
            for (auto OI = I.op_begin(); OI != I.op_end(); OI++) {
                Value *_value = *OI;
                if (definedInCaller(
                        SetVector<BasicBlock *>(_loop->blocks().begin(),
                                                _loop->blocks().end()),
                        _value)) {
                    auto new_live_in = _loop_node->insertLiveInArgument(_value);

                    if (map_value_node.find(_value) == map_value_node.end())
                        assert(!"Couldn't find the live-in source");
                    if (map_value_node.find(&I) == map_value_node.end())
                        assert(!"Couldn't find the live-in target");

                    auto _src = map_value_node[_value];
                    auto _tar = map_value_node[&I];

                    // TODO later we need to get ride of these lines
                    if (auto call_out = dyn_cast<CallNode>(_tar))
                        _tar = call_out->getCallOut();
                    if (auto call_in = dyn_cast<CallNode>(_src))
                        _src = call_in->getCallIn();

                    if (!_src->existDataOutput(new_live_in)) {
                        _src->replaceDataOutputNode(_tar, new_live_in);
                        _tar->replaceDataInputNode(_src, new_live_in);
                        new_live_in->addDataInputPort(_src);
                    } else {
                        _src->removeNodeDataOutputNode(_tar);
                        _tar->replaceDataInputNode(_src, new_live_in);
                    }

                    new_live_in->addDataOutputPort(_tar);
                }
            }
        }
    }
}

void UpdateLiveOutConnections(Loop *_loop, LoopNode *_loop_node,
                              std::map<llvm::Value *, Node *> map_value_node) {
    for (auto B : _loop->blocks()) {
        for (auto &I : *B) {
            /**
             * The function needs these steps:
             * 1) Detect Live-outs
             * 2) Insert a new Live-outs into loop header
             * 3) Update the dependencies
             */
            for (auto *U : I.users()) {
                if (!definedInRegion(
                        SetVector<BasicBlock *>(_loop->blocks().begin(),
                                                _loop->blocks().end()),
                        U)) {
                    auto new_live_out = _loop_node->insertLiveOutArgument(U);

                    auto _src = map_value_node[&I];
                    auto _tar = map_value_node[U];

                    // TODO later we need to get ride of these lines
                    if (auto call_out = dyn_cast<CallNode>(_tar))
                        _tar = call_out->getCallOut();
                    if (auto call_in = dyn_cast<CallNode>(_src))
                        _src = call_in->getCallIn();

                    _src->replaceDataOutputNode(_tar, new_live_out);
                    _tar->replaceDataInputNode(_src, new_live_out);
                    new_live_out->addDataInputPort(_src);

                    new_live_out->addDataOutputPort(_tar);
                }
            }
        }
    }
}

void UpdateInnerLiveInConnections(
    Loop *_loop, std::map<llvm::Loop *, LoopNode *> loop_value_node,
    std::map<llvm::Value *, Node *> map_value_node) {
    for (auto B : _loop->blocks()) {
        for (auto &I : *B) {
            for (auto OI = I.op_begin(); OI != I.op_end(); OI++) {
                Value *_value = *OI;
                if (definedInCaller(
                        SetVector<BasicBlock *>(_loop->blocks().begin(),
                                                _loop->blocks().end()),
                        _value)) {
                    auto _loop_node = loop_value_node[_loop];
                    auto _parent_loop_node =
                        loop_value_node[_loop->getParentLoop()];

                    auto new_live_in = _loop_node->insertLiveInArgument(_value);

                    Node *_src = nullptr;
                    if (_parent_loop_node->findLiveIn(_value) == nullptr)
                        _src = map_value_node[_value];
                    else
                        _src = _parent_loop_node->findLiveIn(_value);
                    auto _tar = map_value_node[&I];

                    // TODO later we need to get ride of these lines
                    if (auto call_out = dyn_cast<CallNode>(_tar))
                        _tar = call_out->getCallOut();
                    if (auto call_in = dyn_cast<CallNode>(_src))
                        _src = call_in->getCallIn();

                    if (!_src->existDataOutput(new_live_in)) {
                        _src->replaceDataOutputNode(_tar, new_live_in);
                        _tar->replaceDataInputNode(_src, new_live_in);
                        new_live_in->addDataInputPort(_src);
                    } else {
                        _src->removeNodeDataOutputNode(_tar);
                        _tar->replaceDataInputNode(_src, new_live_in);
                    }

                    new_live_in->addDataOutputPort(_tar);
                }
            }
        }
    }
}

void UpdateInnerLiveOutConnections(
    Loop *_loop, std::map<llvm::Loop *, LoopNode *> loop_value_node,
    std::map<llvm::Value *, Node *> map_value_node) {
    for (auto B : _loop->blocks()) {
        for (auto &I : *B) {
            auto _loop_node = loop_value_node[_loop];
            auto _parent_loop_node = loop_value_node[_loop->getParentLoop()];

            /**
             * The function needs these steps:
             * 1) Detect Live-outs
             * 2) Insert a new Live-outs into loop header
             * 3) Update the dependencies
             */
            for (auto *U : I.users()) {
                if (!definedInRegion(
                        SetVector<BasicBlock *>(_loop->blocks().begin(),
                                                _loop->blocks().end()),
                        U)) {
                    auto new_live_out = _loop_node->insertLiveOutArgument(U);

                    auto _src = map_value_node[&I];
                    auto _tar = map_value_node[U];

                    // auto _tar = _parent_loop_node->findLiveOut(U);
                    // Node * _tar = nullptr;
                    // if(_parent_loop_node->findLiveOut(U) == nullptr)
                    //_tar = map_value_node[U];
                    // else
                    //_tar = _parent_loop_node->findLiveOut(U);

                    // TODO later we need to get ride of these lines
                    if (auto call_out = dyn_cast<CallNode>(_tar))
                        _tar = call_out->getCallOut();
                    if (auto call_in = dyn_cast<CallNode>(_src))
                        _src = call_in->getCallIn();

                    _src->replaceDataOutputNode(_tar, new_live_out);
                    _tar->replaceDataInputNode(_src, new_live_out);
                    new_live_out->addDataInputPort(_src);
                    new_live_out->addDataOutputPort(_tar);
                }
            }
        }
    }
}

bool GraphGeneratorPass::doInitialization(Module &M) {
    for (auto &F : M) {
        if (F.isDeclaration()) continue;

        if (F.getName() == XKETCHName) {
            this->LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
        }
    }
    // TODO: Add code here if it's needed before pas

    return false;
}

bool GraphGeneratorPass::doFinalization(Module &M) {
    // TODO: Add code here to do post pass
    return false;
}

/**
 * Set pass dependencies
 */
void GraphGeneratorPass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<helpers::GepInformation>();
    AU.addRequired<llvm::LoopInfoWrapperPass>();
    AU.setPreservesAll();
}

/**
 * Iterating over target function's basicblocks and
 * then make supernode for each of them and add them to the node list
 */
void GraphGeneratorPass::visitBasicBlock(BasicBlock &BB) {
    map_value_node[&BB] = this->dependency_graph->insertSuperNode(BB);
}

void GraphGeneratorPass::visitInstruction(Instruction &Ins) {
    // Here we have to check see whether we have missed any instruction or not
    // TODO Couldn't figure out about Tapir visitor functions
    if (auto _detach_ins = dyn_cast<llvm::DetachInst>(&Ins))
        map_value_node[&Ins] =
            this->dependency_graph->insertDetachNode(*_detach_ins);
    else if (auto _reattach_ins = dyn_cast<llvm::ReattachInst>(&Ins))
        map_value_node[&Ins] =
            this->dependency_graph->insertReattachNode(*_reattach_ins);
    else if (auto _sync_ins = dyn_cast<llvm::SyncInst>(&Ins))
        map_value_node[&Ins] =
            this->dependency_graph->insertSyncNode(*_sync_ins);
    else {
        Ins.print(errs(), true);
        assert(!"Instruction is not supported");
    }
}

void GraphGeneratorPass::visitFAdd(llvm::BinaryOperator &I) {
    map_value_node[&I] = this->dependency_graph->insertFaddNode(I);
}

void GraphGeneratorPass::visitFDiv(llvm::BinaryOperator &I) {
    map_value_node[&I] = this->dependency_graph->insertFdiveNode(I);
}

void GraphGeneratorPass::visitBinaryOperator(llvm::BinaryOperator &I) {
    map_value_node[&I] = this->dependency_graph->insertBinaryOperatorNode(I);
}

void GraphGeneratorPass::visitICmpInst(llvm::ICmpInst &I) {
    map_value_node[&I] = this->dependency_graph->insertIcmpOperatorNode(I);
}

void GraphGeneratorPass::visitBranchInst(llvm::BranchInst &I) {
    map_value_node[&I] = this->dependency_graph->insertBranchNode(I);
}

void GraphGeneratorPass::visitPHINode(llvm::PHINode &I) {
    map_value_node[&I] = this->dependency_graph->insertPhiNode(I);
}

void GraphGeneratorPass::visitSelectInst(llvm::SelectInst &I) {
    map_value_node[&I] = this->dependency_graph->insertSelectNode(I);
}

void GraphGeneratorPass::visitFCmp(llvm::FCmpInst &I) {
    map_value_node[&I] = this->dependency_graph->insertFcmpNode(I);
}

void GraphGeneratorPass::visitAllocaInst(llvm::AllocaInst &I) {
    auto alloca_type = I.getAllocatedType();
    auto DL = I.getModule()->getDataLayout();
    auto num_byte = DL.getTypeAllocSize(alloca_type);
    uint32_t size = 1;

    if (alloca_type->isIntegerTy() || alloca_type->isArrayTy()) {
        map_value_node[&I] =
            this->dependency_graph->insertAllocaNode(I, size, num_byte);
    } else if (alloca_type->isPointerTy()) {
        map_value_node[&I] =
            this->dependency_graph->insertAllocaNode(I, size, num_byte);
        I.print(errs(), true);
        errs() << "Alloca is pointer\n";
        // assert(!"Don't support for this alloca");
    } else {
        I.print(errs(), true);
        assert(!"Don't support for this alloca");
    }
}

void GraphGeneratorPass::visitGetElementPtrInst(llvm::GetElementPtrInst &I) {
    auto &gep_pass_ctx = getAnalysis<helpers::GepInformation>();

    auto src_type = I.getSourceElementType();
    if (src_type->isStructTy()) {
        map_value_node[&I] = this->dependency_graph->insertGepNode(
            I, gep_pass_ctx.GepStruct[&I]);
    } else if (src_type->isArrayTy() || src_type->isIntegerTy()) {
        map_value_node[&I] =
            this->dependency_graph->insertGepNode(I, gep_pass_ctx.GepArray[&I]);
    } else
        map_value_node[&I] =
            this->dependency_graph->insertGepNode(I, GepArrayInfo());
}

void GraphGeneratorPass::visitLoadInst(llvm::LoadInst &I) {
    map_value_node[&I] = this->dependency_graph->insertLoadNode(I);
}

void GraphGeneratorPass::visitBitCastInst(llvm::BitCastInst &I) {
    map_value_node[&I] = this->dependency_graph->insertBitcastNode(I);
}

void GraphGeneratorPass::visitStoreInst(llvm::StoreInst &I) {
    map_value_node[&I] = this->dependency_graph->insertStoreNode(I);
}

void GraphGeneratorPass::visitReturnInst(llvm::ReturnInst &I) {
    map_value_node[&I] = this->dependency_graph->insertReturnNode(I);
}

void GraphGeneratorPass::visitCallInst(llvm::CallInst &I) {
    map_value_node[&I] = this->dependency_graph->insertCallNode(I);
}

void GraphGeneratorPass::visitFunction(Function &F) {
    dependency_graph->setFunction(&F);

    // TODO
    // Here we make a graph
    // Filling function argument nodes
    for (auto &f_arg : F.args()) {
        map_value_node[&f_arg] =
            this->dependency_graph->getSplitCall()->insertLiveInArgument(
                &f_arg);
    }

    // this->dependency_graph.setNumSplitCallInput(F.arg_size());

    // Filling function global nodes
    for (auto &g_var : F.getParent()->getGlobalList()) {
        map_value_node[&g_var] =
            this->dependency_graph->insertFunctionGlobalValue(g_var);
    }
}

/**
 * In this function we iterate over each function argument and connect all of
 * its
 * successors as a data input port.
 * If the input is constant value we find the value and make a ConstNode for
 * that value
 */
void GraphGeneratorPass::findDataPort(Function &F) {
    // Check wether we already have iterated over the instructions
    assert(map_value_node.size() > 0 && "Instruction map can not be empty!");

    // Connecting function arguments to the spliter
    for (auto _fun_arg_it = this->dependency_graph->funarg_begin();
         _fun_arg_it != this->dependency_graph->funarg_end(); _fun_arg_it++) {
        auto _fun_arg_node = dyn_cast<ArgumentNode>(_fun_arg_it->get());
        auto _spliter = this->dependency_graph->getSplitCall();

        auto src_idx = _spliter->addDataOutputPort(_fun_arg_node);
        auto dst_idx = _fun_arg_node->addDataInputPort(_spliter);
    }

    for (auto ins_it = inst_begin(F); ins_it != inst_end(F); ++ins_it) {
        for (uint32_t c = 0; c < ins_it->getNumOperands(); ++c) {
            auto operand = ins_it->getOperand(c);

            // First operand of a call function is the pointer its self function
            if (auto fn = dyn_cast<llvm::Function>(operand)) continue;
            if (auto target_bb = dyn_cast<llvm::BasicBlock>(operand)) {
                // Here we have to add a new edge from instruction to BB
                // 1) First find the basicblock node
                // 2) Add the bb as a control output
                // 3) Add the ins ass a control input
                auto _node_dest = this->map_value_node.find(
                    operand);  // it should be supernode
                assert(isa<SuperNode>(_node_dest->second) &&
                       "Destination node should be super node!");

                auto _node_src = this->map_value_node.find(
                    &*ins_it);  // it should be Insnode
                assert(isa<InstructionNode>(_node_src->second) &&
                       "Source node should be instruction node!");

                if (isa<ReattachNode>(_node_src->second)) continue;

                // std::variant<BranchNode, DetachNode> _src;
                auto _dst = _node_dest->second;
                Node *_src = nullptr;
                if (isa<BranchNode>(_node_src->second)) {
                    _src = dyn_cast<BranchNode>(_node_src->second);
                    if (ins_it->getNumOperands() == 3) {
                        if (c == 1)
                            dyn_cast<BranchNode>(_src)->setFalseBranch(_dst);
                        else if (c == 2)
                            dyn_cast<BranchNode>(_src)->setTrueBranch(_dst);
                    } else
                        _src->addControlOutputPort(_dst);

                    _dst->addControlInputPort(_src);

                } else if (isa<DetachNode>(_node_src->second)) {
                    // TODO fix the Detachnode connections
                    //
                    _src = dyn_cast<DetachNode>(_node_src->second);
                    // if (ins_it->getNumOperands() == 3) {
                    // if (c == 1)
                    // dyn_cast<BranchNode>(_src)->setFalseBranch(_dst);
                    // else if (c == 2)
                    // dyn_cast<BranchNode>(_src)->setTrueBranch(_dst);
                    //} else
                    _src->addControlOutputPort(_dst);

                    _dst->addControlInputPort(_src);

                } else if (isa<ReattachNode>(_node_src->second)) {
                    // TODO fix the Reattachnode connections
                    _src = dyn_cast<ReattachNode>(_node_src->second);
                    // if (ins_it->getNumOperands() == 3) {
                    // if (c == 1)
                    // dyn_cast<BranchNode>(_src)->setFalseBranch(_dst);
                    // else if (c == 2)
                    // dyn_cast<BranchNode>(_src)->setTrueBranch(_dst);
                    //} else
                    _src->addControlOutputPort(_dst);

                    _dst->addControlInputPort(_src);

                } else if (isa<SyncNode>(_node_src->second)) {
                    // TODO fix the Sync node connections
                    _src = dyn_cast<SyncNode>(_node_src->second);
                    // if (ins_it->getNumOperands() == 3) {
                    // if (c == 1)
                    // dyn_cast<BranchNode>(_src)->setFalseBranch(_dst);
                    // else if (c == 2)
                    // dyn_cast<BranchNode>(_src)->setTrueBranch(_dst);
                    //} else
                    _src->addControlOutputPort(_dst);

                    _dst->addControlInputPort(_src);

                } else {
                    assert(!"Wrong cast of control node!");
                }

            } else {
                // If the operand is constant we have to create a new node
                if (isa<llvm::AllocaInst>(&*ins_it)) continue;

                Node *_const_node = nullptr;
                if (auto const_value = dyn_cast<llvm::ConstantInt>(operand)) {
                    _const_node = this->dependency_graph->insertConstIntNode(
                        *const_value);
                    map_value_node[operand] = _const_node;

                    _const_node->addControlInputPort(
                        this->map_value_node[ins_it->getParent()]);
                    this->map_value_node[ins_it->getParent()]
                        ->addControlOutputPort(_const_node);
                    dyn_cast<SuperNode>(
                        this->map_value_node[ins_it->getParent()])
                        ->addconstIntNode(dyn_cast<ConstIntNode>(_const_node));
                } else if (auto const_value =
                               dyn_cast<llvm::ConstantFP>(operand)) {
                    _const_node =
                        this->dependency_graph->insertConstFPNode(*const_value);
                    map_value_node[operand] = _const_node;

                    _const_node->addControlInputPort(
                        this->map_value_node[ins_it->getParent()]);
                    this->map_value_node[ins_it->getParent()]
                        ->addControlOutputPort(_const_node);
                    dyn_cast<SuperNode>(
                        this->map_value_node[ins_it->getParent()])
                        ->addconstFPNode(dyn_cast<ConstFPNode>(_const_node));
                } else if (auto undef_value =
                               dyn_cast<llvm::UndefValue>(operand)) {
                    _const_node = this->dependency_graph->insertConstIntNode();
                    map_value_node[operand] = _const_node;

                    _const_node->addControlInputPort(
                        this->map_value_node[ins_it->getParent()]);
                    this->map_value_node[ins_it->getParent()]
                        ->addControlOutputPort(_const_node);
                    dyn_cast<SuperNode>(
                        this->map_value_node[ins_it->getParent()])
                        ->addconstIntNode(dyn_cast<ConstIntNode>(_const_node));
                }

                auto _node_src = this->map_value_node.find(operand);
                auto _node_dest = this->map_value_node.find(&*ins_it);

                if (_node_src == this->map_value_node.end()) {
                    DEBUG(operand->print(errs(), true));
                    DEBUG(errs() << "\n");
                    DEBUG(ins_it->print(errs(), true));
                    assert(!"The destination instruction couldn't find!");
                }

                if (_node_dest == this->map_value_node.end()) {
                    DEBUG(operand->print(errs(), true));
                    DEBUG(errs() << "\n");
                    DEBUG(ins_it->print(errs(), true));
                    assert(!"The destination instruction couldn't find!");
                }

                auto _src = _node_src->second;
                auto _dst = _node_dest->second;

                if (_const_node) _src = _const_node;
                if (auto call_out = dyn_cast<CallNode>(_dst))
                    _dst = call_out->getCallOut();
                if (auto call_in = dyn_cast<CallNode>(_src))
                    _src = call_in->getCallIn();

                _src->addDataOutputPort(_dst);
                _dst->addDataInputPort(_src);
            }
        }

        // Connecting LD and ST nodes to Memory system
        if (auto _ld_node = dyn_cast<LoadNode>(
                this->map_value_node.find(&*ins_it)->second)) {
            // TODO right now we consider all the connections to the cache or
            // regfile
            // We need a pass to trace the pointers
            auto _dst_req_idx =
                this->dependency_graph->getMemoryUnit()->addReadMemoryReqPort(
                    _ld_node);
            auto _src_resp_idx =
                this->dependency_graph->getMemoryUnit()->addReadMemoryRespPort(
                    _ld_node);
            auto _src_req_idx = _ld_node->addReadMemoryReqPort(
                this->dependency_graph->getMemoryUnit());
            auto _dst_resp_idx = _ld_node->addReadMemoryRespPort(
                this->dependency_graph->getMemoryUnit());

        } else if (auto _st_node = dyn_cast<StoreNode>(
                       this->map_value_node.find(&*ins_it)->second)) {
            auto _dst_req_idx =
                this->dependency_graph->getMemoryUnit()->addWriteMemoryReqPort(
                    _st_node);
            auto _src_resp_idx =
                this->dependency_graph->getMemoryUnit()->addWriteMemoryRespPort(
                    _st_node);
            auto _src_req_idx = _st_node->addWriteMemoryReqPort(
                this->dependency_graph->getMemoryUnit());
            auto _dst_resp_idx = _st_node->addWriteMemoryRespPort(
                this->dependency_graph->getMemoryUnit());
        } else if (auto _alloca_node = dyn_cast<AllocaNode>(
                       this->map_value_node.find(&*ins_it)->second)) {
            auto _dst_req_idx = this->dependency_graph->getStackAllocator()
                                    ->addReadMemoryReqPort(_alloca_node);
            auto _src_resp_idx = this->dependency_graph->getStackAllocator()
                                     ->addReadMemoryRespPort(_alloca_node);
            auto _src_req_idx = _alloca_node->addReadMemoryReqPort(
                this->dependency_graph->getStackAllocator());
            auto _dst_resp_idx = _alloca_node->addReadMemoryRespPort(
                this->dependency_graph->getStackAllocator());
        } else if (auto _fpdiv_node = dyn_cast<FdiveOperatorNode>(
                       this->map_value_node.find(&*ins_it)->second)) {
            auto _dst_req_idx =
                this->dependency_graph->getFPUNode()->addReadMemoryReqPort(
                    _fpdiv_node);
            auto _src_resp_idx =
                this->dependency_graph->getFPUNode()->addReadMemoryRespPort(
                    _fpdiv_node);
            auto _src_req_idx = _fpdiv_node->addReadMemoryReqPort(
                this->dependency_graph->getFPUNode());
            auto _dst_resp_idx = _fpdiv_node->addReadMemoryRespPort(
                this->dependency_graph->getFPUNode());
        }
    }
}

/**
 * This function has two tasks:
 * 1) Iterate over the basicblock's insturcitons and make a list of instructions
 * 2) Make control dependnce edges
 */
void GraphGeneratorPass::fillBasicBlockDependencies(Function &F) {
    // Find the entry basic block and connect it to the splitnode
    for (auto &BB : F) {
        // Find the entry basic block and connect it to the split node
        if (&BB == &F.getEntryBlock()) {
            auto _en_bb = dyn_cast<SuperNode>(this->map_value_node[&BB]);
            auto _src_idx =
                this->dependency_graph->getSplitCall()->addControlOutputPort(
                    _en_bb);
            auto _dst_idx = _en_bb->addControlInputPort(
                this->dependency_graph->getSplitCall());
        }
        // if (returnNumPred(&BB) > 1) {
        if (dyn_cast<SuperNode>(this->map_value_node[&BB])
                ->numControlInputPort() > 1) {
            auto _bb = dyn_cast<SuperNode>(this->map_value_node[&BB]);
            _bb->setNodeType(SuperNode::Mask);
        }

        if (auto _bb = dyn_cast<SuperNode>(this->map_value_node[&BB])) {
            for (auto &I : BB) {
                // Iterate over the basicblock's instructions
                if (auto _ins =
                        dyn_cast<InstructionNode>(this->map_value_node[&I])) {
                    _bb->addInstruction(_ins);

                    if (auto reatach = dyn_cast<ReattachNode>(_ins)) continue;

                    // Detect Phi instrucctions
                    if (auto _phi_ins = dyn_cast<PhiSelectNode>(_ins)) {
                        //_bb->setNodeType(SuperNode::Mask);
                        _bb->addPhiInstruction(_phi_ins);
                        _phi_ins->setParentNode(_bb);
                    }

                    if (auto _call_node = dyn_cast<CallNode>(_ins)) {
                        _bb->addControlOutputPort(_call_node->getCallOut());
                        _call_node->setCallOutEnable(_bb);
                    } else {
                        // Make a control edge
                        _bb->addControlOutputPort(_ins);
                        _ins->addControlInputPort(_bb);
                    }

                } else
                    assert(!"The instruction is not visited!");
            }

        } else
            assert(!"The basicblock is not visited!");
    }
}

/**
 * This funciton iterates over function loops and generate loop nodes
 */
[[deprecated("Replace by updateLoopDependencies")]] void
GraphGeneratorPass::fillLoopDependencies(llvm::LoopInfo &loop_info) {
    uint32_t c = 0;
    for (auto &L : getLoops(loop_info)) {
        LoopNode *_parent = nullptr;
        if (L->getParentLoop()) _parent = loop_value_node[L->getParentLoop()];
        auto _new_loop = std::make_unique<LoopNode>(
            NodeInfo(c, "Loop_" + std::to_string(c)));

        // Insert the loop node
        auto _loop_node =
            this->dependency_graph->insertLoopNode(std::move(_new_loop));
        auto _l_head = dyn_cast<SuperNode>(map_value_node[L->getHeader()]);
        auto _l_exit = dyn_cast<SuperNode>(map_value_node[L->getExitBlock()]);

        // Change the type of loop head basic block
        _l_head->setNodeType(SuperNode::SuperNodeType::LoopHead);

        auto _src_br_inst_it = *std::find_if(
            _l_head->inputControl_begin(), _l_head->inputControl_end(),
            [&L](auto const _node_it) {
                return !L->contains(
                    dyn_cast<BranchNode>(_node_it)->getInstruction());
            });

        _loop_node->setEnableLoopSignal(_src_br_inst_it);
        _loop_node->setActiveOutputLoopSignal(_l_head);
        _loop_node->setLoopEndEnable(_l_exit);

        _src_br_inst_it->replaceControlOutputNode(_l_head, _loop_node);
        _l_head->replaceControlInputNode(_src_br_inst_it, _loop_node);

        // Connect the latch ending branch to loopNode
        if (auto _latch_br = dyn_cast<BranchNode>(
                map_value_node[&L->getLoopLatch()->back()])) {
            auto _src_idx = _latch_br->addControlOutputPort(_loop_node);
            _loop_node->setLoopLatchEnable(_latch_br);

        } else
            assert(!"Unexpected terminator!");

        // Connecting end branch to the loop end input
        auto _tar_exit_br_inst_it = *std::find_if(
            _l_exit->inputControl_begin(), _l_exit->inputControl_end(),
            [&L](auto const _node_it) {
                return L->contains(
                    dyn_cast<BranchNode>(_node_it)->getInstruction());
            });

        auto _src_idx = _loop_node->pushLoopExitLatch(_tar_exit_br_inst_it);

        _tar_exit_br_inst_it->replaceControlOutputNode(_l_exit, _loop_node);
        _l_exit->replaceControlInputNode(_tar_exit_br_inst_it, _loop_node);

        // Update data dependencies
        /**
         * The function needs these steps:
         * 1) Detect Live-ins
         * 2) Insert a new Live-in into loop header
         * 3) Update the dependencies
         */
        for (auto B : L->blocks()) {
            UpdateLiveInConnections(L, _loop_node, map_value_node);
            for (auto &I : *B) {
                /**
                 * The function needs these steps:
                 * 1) Detect Live-outs
                 * 2) Insert a new Live-outs into loop header
                 * 3) Update the dependencies
                 */
                for (auto *U : I.users()) {
                    if (!definedInRegion(
                            SetVector<BasicBlock *>(L->blocks().begin(),
                                                    L->blocks().end()),
                            U)) {
                        auto new_live_out =
                            _loop_node->insertLiveOutArgument(U);

                        auto _src = map_value_node[&I];
                        auto _tar = map_value_node[U];

                        // TODO later we need to get ride of these lines
                        if (auto call_out = dyn_cast<CallNode>(_tar))
                            _tar = call_out->getCallOut();
                        if (auto call_in = dyn_cast<CallNode>(_src))
                            _src = call_in->getCallIn();

                        _src->replaceDataOutputNode(_tar, new_live_out);
                        _tar->replaceDataInputNode(_src, new_live_out);
                        new_live_out->addDataInputPort(_src);

                        new_live_out->addDataOutputPort(_tar);
                    }
                }
            }
        }

        // Increament the counter
        c++;

        // Filling the containers
        for (auto B : L->blocks()) {
            if (!B->empty()) {
                _loop_node->pushSuperNode(
                    dyn_cast<SuperNode>(map_value_node[B]));
                for (auto &I : *B) {
                    _loop_node->pushInstructionNode(
                        dyn_cast<InstructionNode>(map_value_node[&I]));
                }
            }
        }
        // This function should be called after filling the containers always
        _loop_node->setEndingInstructions();
        for (auto _en_instruction : _loop_node->endings()) {
            auto _en = _en_instruction->getInstruction();
            auto &_br_ins = map_value_node[&_en_instruction->getInstruction()
                                                ->getParent()
                                                ->getInstList()
                                                .back()];

            _en_instruction->addControlOutputPort(_br_ins);
            _br_ins->addControlInputPort(_en_instruction);
        }
    }
}

/**
 * This funciton iterates over function loops and generate loop nodes
 */
void GraphGeneratorPass::updateLoopDependencies(llvm::LoopInfo &loop_info) {
    uint32_t c = 0;

    for (auto &L : getLoops(loop_info)) {
        // DEBUG
        L->getHeader()->getName();
        // Getting list of loop's exit basicblock
        SmallVector<BasicBlock *, 8> _exit_blocks;
        L->getExitBlocks(_exit_blocks);

        auto _l_head = dyn_cast<SuperNode>(map_value_node[L->getHeader()]);
        std::vector<std::pair<BasicBlock *, SuperNode *>> _l_exit_blocks;

        for (auto _l : _exit_blocks) {
            _l_exit_blocks.push_back(
                std::make_pair(_l, dyn_cast<SuperNode>(map_value_node[_l])));
        }

        // Change the type of loop head basic block
        _l_head->setNodeType(SuperNode::SuperNodeType::LoopHead);

        auto _src_br_inst_it = *std::find_if(
            _l_head->inputControl_begin(), _l_head->inputControl_end(),
            [&L](auto const _node_it) {
                return !L->contains(
                    dyn_cast<BranchNode>(_node_it)->getInstruction());
            });

        std::list<SuperNode *> _list_exit;
        std::transform(_l_exit_blocks.begin(), _l_exit_blocks.end(),
                       std::back_inserter(_list_exit),
                       [](auto _l_e) -> SuperNode * { return _l_e.second; });

        auto _new_loop = std::make_unique<LoopNode>(
            NodeInfo(c, "Loop_" + std::to_string(c)),
            dyn_cast<SuperNode>(map_value_node[L->getHeader()]),
            dyn_cast<SuperNode>(map_value_node[L->getLoopLatch()]), _list_exit);

        // Insert the loop node
        auto _loop_node =
            this->dependency_graph->insertLoopNode(std::move(_new_loop));

        loop_value_node[&*L] = _loop_node;

        _loop_node->setEnableLoopSignal(_src_br_inst_it);
        _loop_node->setActiveOutputLoopSignal(_l_head);

        uint32_t _index = 0;
        for (auto _l : _list_exit) {
            _loop_node->setLoopEndEnable(_l, _index++);
        }
        //        _loop_node->setLoopEndEnable(_l_exit);

        _src_br_inst_it->replaceControlOutputNode(_l_head, _loop_node);
        _l_head->replaceControlInputNode(_src_br_inst_it, _loop_node);

        // Connect the latch ending branch to loopNode
        if (auto _latch_br = dyn_cast<BranchNode>(
                map_value_node[&L->getLoopLatch()->back()])) {
            auto _src_idx = _latch_br->addControlOutputPort(_loop_node);
            _loop_node->setLoopLatchEnable(_latch_br);

        } else
            assert(!"Unexpected terminator!");

        // Connecting end branch to the loop end input
        for (auto _le : _list_exit) {
            auto _tar_exit_br_inst_it = *std::find_if(
                _le->inputControl_begin(), _le->inputControl_end(),
                [&L](auto const _node_it) {

                    if (_node_it == nullptr) return false;

                    if (dyn_cast<BranchNode>(_node_it))
                        return L->contains(
                            dyn_cast<BranchNode>(_node_it)->getInstruction());
                    else
                        return false;
                });

            auto _src_idx = _loop_node->pushLoopExitLatch(_tar_exit_br_inst_it);

            _tar_exit_br_inst_it->replaceControlOutputNode(_le, _loop_node);
            _le->replaceControlInputNode(_tar_exit_br_inst_it, _loop_node);
        }

        // Increament loop counter
        c++;

    }  // Get loops

    for (auto &L : getLoops(loop_info)) {
        auto _loop_node = loop_value_node[L];

        // Filling loop containers
        for (auto B : L->blocks()) {
            if (!B->empty()) {
                _loop_node->pushSuperNode(
                    dyn_cast<SuperNode>(map_value_node[B]));
                for (auto &I : *B) {
                    _loop_node->pushInstructionNode(
                        dyn_cast<InstructionNode>(map_value_node[&I]));
                }
            }
        }
    }

    for (auto &L : getOuterLoops(loop_info)) {
        auto _loop_node = loop_value_node[L];
        _loop_node->setOuterLoop();

        // This function should be called after filling the containers always
        _loop_node->setEndingInstructions();

        for (auto _en_instruction : _loop_node->endings()) {
            auto _en = _en_instruction->getInstruction();
            auto &_br_ins = map_value_node[&_en_instruction->getInstruction()
                                                ->getParent()
                                                ->getInstList()
                                                .back()];

            _en_instruction->addControlOutputPort(_br_ins);
            _br_ins->addControlInputPort(_en_instruction);
        }
    }

    for (auto &L : getOuterLoops(loop_info)) {
        // At this stage we know that outer loop dominante all other loops
        // therefore each live-in for subLoops is a live-in for the outer
        // loop as well. We first connect all the live-ins to the outer loop
        // and then iteratively go trought the subloops and update the
        // connections.
        auto _loop_node = loop_value_node[&*L];
        UpdateLiveInConnections(L, _loop_node, map_value_node);
        UpdateLiveOutConnections(L, _loop_node, map_value_node);

        std::queue<Loop *> _loop_queue;

        for (auto _l : L->getSubLoopsVector()) _loop_queue.push(_l);

        while (!_loop_queue.empty()) {
            auto _sub_loop = _loop_queue.front();
            _loop_queue.pop();

            UpdateInnerLiveInConnections(_sub_loop, loop_value_node,
                                         map_value_node);
            UpdateInnerLiveOutConnections(_sub_loop, loop_value_node,
                                          map_value_node);

            for (auto _tmp_sub : _sub_loop->getSubLoopsVector()) {
                _loop_queue.push(_tmp_sub);
            }
        }
    }
}

/**
 * This funciton iterates over function loops and generate loop nodes
 */
void GraphGeneratorPass::makeLoopNodes(llvm::LoopInfo &loop_info) {
    uint32_t c = 0;

    for (auto &L : getLoops(loop_info)) {
        // DEBUG
        L->getHeader()->getName();

        // Getting list of loop's exit basicblock
        SmallVector<BasicBlock *, 8> _exit_blocks;
        L->getExitBlocks(_exit_blocks);

        auto _l_head = dyn_cast<SuperNode>(map_value_node[L->getHeader()]);
        std::vector<std::pair<BasicBlock *, SuperNode *>> _l_exit_blocks;

        for (auto _l : _exit_blocks) {
            _l_exit_blocks.push_back(
                std::make_pair(_l, dyn_cast<SuperNode>(map_value_node[_l])));
        }

        // Change the type of loop head basic block
        _l_head->setNodeType(SuperNode::SuperNodeType::LoopHead);

        auto _src_br_inst_it = *std::find_if(
            _l_head->inputControl_begin(), _l_head->inputControl_end(),
            [&L](auto const _node_it) {
                return !L->contains(
                    dyn_cast<BranchNode>(_node_it)->getInstruction());
            });

        std::list<SuperNode *> _list_exit;
        std::transform(_l_exit_blocks.begin(), _l_exit_blocks.end(),
                       std::back_inserter(_list_exit),
                       [](auto _l_e) -> SuperNode * { return _l_e.second; });

        auto _new_loop = std::make_unique<LoopNode>(
            NodeInfo(c, "Loop_" + std::to_string(c)),
            dyn_cast<SuperNode>(map_value_node[L->getHeader()]),
            dyn_cast<SuperNode>(map_value_node[L->getLoopLatch()]), _list_exit);

        // Insert the loop node
        auto _loop_node =
            this->dependency_graph->insertLoopNode(std::move(_new_loop));

        loop_value_node[&*L] = _loop_node;

        _loop_node->setEnableLoopSignal(_src_br_inst_it);
        _loop_node->setActiveOutputLoopSignal(_l_head);

        uint32_t _index = 0;
        for (auto _l : _list_exit) {
            _loop_node->setLoopEndEnable(_l, _index++);
        }

        //_src_br_inst_it->replaceControlOutputNode(_l_head, _loop_node);
        //_l_head->replaceControlInputNode(_src_br_inst_it, _loop_node);
        _src_br_inst_it->addControlOutputPort(_loop_node);
        _l_head->addControlInputPort(_loop_node);

        // Connect the latch ending branch to loopNode
        if (auto _latch_br = dyn_cast<BranchNode>(
                map_value_node[&L->getLoopLatch()->back()])) {
            auto _src_idx = _latch_br->addControlOutputPort(_loop_node);
            _loop_node->setLoopLatchEnable(_latch_br);

        } else
            assert(!"Unexpected terminator!");

        // Connecting end branch to the loop end input
        for (auto _le : _list_exit) {
            auto _tar_exit_br_inst_it = *std::find_if(
                _le->inputControl_begin(), _le->inputControl_end(),
                [&L](auto const _node_it) {

                    return L->contains(
                        dyn_cast<BranchNode>(_node_it)->getInstruction());
                });

            auto _src_idx = _loop_node->pushLoopExitLatch(_tar_exit_br_inst_it);

            //_tar_exit_br_inst_it->replaceControlOutputNode(_le, _loop_node);
            //_le->replaceControlInputNode(_tar_exit_br_inst_it, _loop_node);
            _tar_exit_br_inst_it->addControlOutputPort(_loop_node);
            _le->addControlInputPort(_loop_node);
        }

        // Increament loop counter
        c++;

    }  // Get loops

    for (auto &L : getLoops(loop_info)) {
        auto _loop_node = loop_value_node[L];

        // Filling loop containers
        for (auto B : L->blocks()) {
            if (!B->empty()) {
                _loop_node->pushSuperNode(
                    dyn_cast<SuperNode>(map_value_node[B]));
                for (auto &I : *B) {
                    _loop_node->pushInstructionNode(
                        dyn_cast<InstructionNode>(map_value_node[&I]));
                }
            }
        }
    }

    for (auto &L : getOuterLoops(loop_info)) {
        auto _loop_node = loop_value_node[L];
        _loop_node->setOuterLoop();

        // This function should be called after filling the containers always
        _loop_node->setEndingInstructions();

        for (auto _en_instruction : _loop_node->endings()) {
            auto _en = _en_instruction->getInstruction();
            auto &_br_ins = map_value_node[&_en_instruction->getInstruction()
                                                ->getParent()
                                                ->getInstList()
                                                .back()];

            _en_instruction->addControlOutputPort(_br_ins);
            _br_ins->addControlInputPort(_en_instruction);
        }
    }

    for (auto &L : getOuterLoops(loop_info)) {
        // At this stage we know that outer loop dominante all other loops
        // therefore each live-in for subLoops is a live-in for the outer
        // loop as well. We first connect all the live-ins to the outer loop
        // and then iteratively go trought the subloops and update the
        // connections.
        auto _loop_node = loop_value_node[&*L];
        UpdateLiveInConnections(L, _loop_node, map_value_node);
        UpdateLiveOutConnections(L, _loop_node, map_value_node);

        std::queue<Loop *> _loop_queue;

        for (auto _l : L->getSubLoopsVector()) _loop_queue.push(_l);

        while (!_loop_queue.empty()) {
            auto _sub_loop = _loop_queue.front();
            _loop_queue.pop();

            UpdateInnerLiveInConnections(_sub_loop, loop_value_node,
                                         map_value_node);
            UpdateInnerLiveOutConnections(_sub_loop, loop_value_node,
                                          map_value_node);

            for (auto _tmp_sub : _sub_loop->getSubLoopsVector()) {
                _loop_queue.push(_tmp_sub);
            }
        }
    }
}

void GraphGeneratorPass::connectOutToReturn(Function &F) {
    for (auto &BB : F) {
        for (auto &I : BB) {
            if (isa<llvm::ReturnInst>(I)) {
                dependency_graph->setOutputNode(map_value_node[&I]);
            }
        }
    }
}

void GraphGeneratorPass::connectParalleNodes(Function &F) {
    if (!findParallelInstruction<llvm::SyncInst>(F)) return;
    auto _sync_node =
        this->map_value_node[findParallelInstruction<llvm::SyncInst>(F)];
    auto _detach_node =
        this->map_value_node[findParallelInstruction<llvm::DetachInst>(F)];
    auto _reattach_node =
        this->map_value_node[findParallelInstruction<llvm::ReattachInst>(F)];

    _sync_node->addControlInputPort(_detach_node);
    _sync_node->addControlInputPort(_reattach_node);
}

void GraphGeneratorPass::connectingCalldependencies(Function &F) {
    auto call_instructions =
        getNodeList<CallNode>(this->dependency_graph.get());
    for (auto _call_node : call_instructions) {
        auto _ins = _call_node->getInstruction();
        auto &_end_ins = _ins->getParent()->back();
        auto _end_node = map_value_node[&_end_ins];
        if (isa<ReattachNode>(_end_node) || isa<BranchNode>(_end_node)) {
            if (isa<BranchNode>(_end_node)) {
                _end_node->addDataInputPort(_call_node->getCallIn());
                _call_node->getCallIn()->addDataOutputPort(_end_node);
            }

            _end_node->addControlInputPort(_call_node->getCallIn());
            _call_node->getCallIn()->addControlOutputPort(_end_node);
        }
    }
}

/**
 * All the initializations for function members
 */
void GraphGeneratorPass::init(Function &F) {
    // Running analysis on the elements
    findDataPort(F);
    fillBasicBlockDependencies(F);

    // fillLoopDependencies(*LI);
    updateLoopDependencies(*LI);
    connectOutToReturn(F);
    connectParalleNodes(F);
    connectingCalldependencies(F);

    // Printing the graph
    dependency_graph->optimizationPasses();
    dependency_graph->printGraph(PrintType::Scala, config_path);
}

// bool GraphGeneratorPass::runOnFunction(Function &F) {
bool GraphGeneratorPass::runOnModule(Module &M) {
    for (auto &F : M) {
        if (F.isDeclaration()) continue;
        if (F.getName() == XKETCHName) {
            stripDebugInfo(F);
            visit(F);
            init(F);
        }
    }

    return false;
}