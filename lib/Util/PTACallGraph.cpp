//===- PTACallGraph.cpp -- Call graph used internally in SVF------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


/*
 * PTACallGraph.cpp
 *
 *  Created on: Nov 7, 2013
 *      Author: Yulei Sui
 */

#include "Util/PTACallGraph.h"
#include "Util/GraphUtil.h"
#include <llvm/Support/DOTGraphTraits.h>	// for dot graph traits
#include <llvm/IR/InstIterator.h>	// for inst iteration

using namespace llvm;
using namespace analysisUtil;


static cl::opt<bool> CallGraphDotGraph("dump-callgraph", cl::init(false),
                                       cl::desc("Dump dot graph of Call Graph"));



bool PTACallGraphNode::isReachableFromProgEntry() const
{
    std::stack<const PTACallGraphNode*> nodeStack;
    NodeBS visitedNodes;
    nodeStack.push(this);
    visitedNodes.set(getId());

    while (nodeStack.empty() == false) {
        PTACallGraphNode* node = const_cast<PTACallGraphNode*>(nodeStack.top());
        nodeStack.pop();

        if (analysisUtil::isProgEntryFunction(node->getFunction()))
            return true;

        for (const_iterator it = node->InEdgeBegin(), eit = node->InEdgeEnd(); it != eit; ++it) {
            PTACallGraphEdge* edge = *it;
            if (visitedNodes.test_and_set(edge->getSrcID()))
                nodeStack.push(edge->getSrcNode());
        }
    }

    return false;
}


/*!
 * Build call graph, connect direct call edge only
 */
void PTACallGraph::buildCallGraph(llvm::Module* module) {

    /// create nodes
    for (Module::iterator F = module->begin(), E = module->end(); F != E; ++F) {
        addCallGraphNode(&*F);
    }

    /// create edges
    for (Module::iterator F = module->begin(), E = module->end(); F != E; ++F) {
        for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
            const Instruction *inst = &*II;
            if (isCallSite(inst) && isInstrinsicDbgInst(inst)==false) {
                if(getCallee(inst))
                    addDirectCallGraphEdge(inst);
            }
        }
    }

    dump("callgraph_initial");
}

/*!
 *  Memory has been cleaned up at GenericGraph
 */
void PTACallGraph::destroy() {
}

/*!
 * Add call graph node
 */
void PTACallGraph::addCallGraphNode(const llvm::Function* fun) {
    NodeID id = callGraphNodeNum;
    PTACallGraphNode* callGraphNode = new PTACallGraphNode(id, fun);
    addGNode(id,callGraphNode);
    funToCallGraphNodeMap[fun] = callGraphNode;
    callGraphNodeNum++;
}

/*!
 *  Whether we have already created this call graph edge
 */
PTACallGraphEdge* PTACallGraph::hasGraphEdge(PTACallGraphNode* src, PTACallGraphNode* dst,PTACallGraphEdge::CEDGEK kind) const {
    PTACallGraphEdge edge(src,dst,kind);
    PTACallGraphEdge* outEdge = src->hasOutgoingEdge(&edge);
    PTACallGraphEdge* inEdge = dst->hasIncomingEdge(&edge);
    if (outEdge && inEdge) {
        assert(outEdge == inEdge && "edges not match");
        return outEdge;
    }
    else
        return NULL;
}

/*!
 * get CallGraph edge via nodes
 */
PTACallGraphEdge* PTACallGraph::getGraphEdge(PTACallGraphNode* src, PTACallGraphNode* dst,PTACallGraphEdge::CEDGEK kind) {
    for (PTACallGraphEdge::CallGraphEdgeSet::iterator iter = src->OutEdgeBegin();
            iter != src->OutEdgeEnd(); ++iter) {
        PTACallGraphEdge* edge = (*iter);
        if (edge->getEdgeKind() == kind && edge->getDstID() == dst->getId())
            return edge;
    }
    return NULL;
}

/*!
 * Add direct call edges
 */
void PTACallGraph::addDirectCallGraphEdge(const llvm::Instruction* call) {
    assert(getCallee(call) && "no callee found");

    PTACallGraphNode* caller = getCallGraphNode(call->getParent()->getParent());
    PTACallGraphNode* callee = getCallGraphNode(getCallee(call));

    if(PTACallGraphEdge* callEdge = hasGraphEdge(caller,callee, PTACallGraphEdge::CallRetEdge)) {
        callEdge->addDirectCallSite(call);
        addCallGraphEdgeSetMap(call,callEdge);
    }
    else {
        assert(call->getParent()->getParent() == caller->getFunction()
               && "callee instruction not inside caller??");

        PTACallGraphEdge* edge = new PTACallGraphEdge(caller,callee,PTACallGraphEdge::CallRetEdge);
        edge->addDirectCallSite(call);

        addEdge(edge);
        addCallGraphEdgeSetMap(call,edge);
    }
}

/*!
 * Add indirect call edge to update call graph
 */
void PTACallGraph::addIndirectCallGraphEdge(const llvm::Instruction* call, const llvm::Function* calleefun) {
    PTACallGraphNode* caller = getCallGraphNode(call->getParent()->getParent());
    PTACallGraphNode* callee = getCallGraphNode(calleefun);

    numOfResolvedIndCallEdge++;

    if(PTACallGraphEdge* callEdge = hasGraphEdge(caller,callee, PTACallGraphEdge::CallRetEdge)) {
        callEdge->addInDirectCallSite(call);
        addCallGraphEdgeSetMap(call,callEdge);
    }
    else {
        assert(call->getParent()->getParent() == caller->getFunction()
               && "callee instruction not inside caller??");

        PTACallGraphEdge* edge = new PTACallGraphEdge(caller,callee,PTACallGraphEdge::CallRetEdge);
        edge->addInDirectCallSite(call);

        addEdge(edge);
        addCallGraphEdgeSetMap(call,edge);
    }
}

/*!
 * Get all callsite invoking this callee
 */
void PTACallGraph::getAllCallSitesInvokingCallee(const llvm::Function* callee, PTACallGraphEdge::CallInstSet& csSet) {
    PTACallGraphNode* callGraphNode = getCallGraphNode(callee);
    for(PTACallGraphNode::iterator it = callGraphNode->InEdgeBegin(), eit = callGraphNode->InEdgeEnd();
            it!=eit; ++it) {
        for(PTACallGraphEdge::CallInstSet::iterator cit = (*it)->directCallsBegin(),
                ecit = (*it)->directCallsEnd(); cit!=ecit; ++cit) {
            csSet.insert((*cit));
        }
        for(PTACallGraphEdge::CallInstSet::iterator cit = (*it)->indirectCallsBegin(),
                ecit = (*it)->indirectCallsEnd(); cit!=ecit; ++cit) {
            csSet.insert((*cit));
        }
    }
}

/*!
 * Get direct callsite invoking this callee
 */
void PTACallGraph::getDirCallSitesInvokingCallee(const llvm::Function* callee, PTACallGraphEdge::CallInstSet& csSet) {
    PTACallGraphNode* callGraphNode = getCallGraphNode(callee);
    for(PTACallGraphNode::iterator it = callGraphNode->InEdgeBegin(), eit = callGraphNode->InEdgeEnd();
            it!=eit; ++it) {
        for(PTACallGraphEdge::CallInstSet::iterator cit = (*it)->directCallsBegin(),
                ecit = (*it)->directCallsEnd(); cit!=ecit; ++cit) {
            csSet.insert((*cit));
        }
    }
}

/*!
 * Get indirect callsite invoking this callee
 */
void PTACallGraph::getIndCallSitesInvokingCallee(const llvm::Function* callee, PTACallGraphEdge::CallInstSet& csSet) {
    PTACallGraphNode* callGraphNode = getCallGraphNode(callee);
    for(PTACallGraphNode::iterator it = callGraphNode->InEdgeBegin(), eit = callGraphNode->InEdgeEnd();
            it!=eit; ++it) {
        for(PTACallGraphEdge::CallInstSet::iterator cit = (*it)->indirectCallsBegin(),
                ecit = (*it)->indirectCallsEnd(); cit!=ecit; ++cit) {
            csSet.insert((*cit));
        }
    }
}

/*!
 * Issue a warning if the function which has indirect call sites can not be reached from program entry.
 */
void PTACallGraph::vefityCallGraph()
{
    CallEdgeMap::const_iterator it = indirectCallMap.begin();
    CallEdgeMap::const_iterator eit = indirectCallMap.end();
    for (; it != eit; ++it) {
        const FunctionSet& targets = it->second;
        if (targets.empty() == false) {
            CallSite cs = it->first;
            const Function* func = cs.getInstruction()->getParent()->getParent();
            if (getCallGraphNode(func)->isReachableFromProgEntry() == false)
                wrnMsg(func->getName().str() + " has indirect call site but not reachable from main");
        }
    }
}

/*!
 * Dump call graph into dot file
 */
void PTACallGraph::dump(const std::string& filename) {
    if(CallGraphDotGraph)
        GraphPrinter::WriteGraphToFile(llvm::outs(), filename, this);

}


namespace llvm {

/*!
 * Write value flow graph into dot file for debugging
 */
template<>
struct DOTGraphTraits<PTACallGraph*> : public DefaultDOTGraphTraits {

    typedef PTACallGraphNode NodeType;
    typedef NodeType::iterator ChildIteratorType;
    DOTGraphTraits(bool isSimple = false) :
        DefaultDOTGraphTraits(isSimple) {
    }

    /// Return name of the graph
    static std::string getGraphName(PTACallGraph *graph) {
        return "Call Graph";
    }
    /// Return function name;
    static std::string getNodeLabel(PTACallGraphNode *node, PTACallGraph *graph) {
        return node->getFunction()->getName().str();
    }

    static std::string getNodeAttributes(PTACallGraphNode *node, PTACallGraph *PTACallGraph) {
        const Function* fun = node->getFunction();
        if (!analysisUtil::isExtCall(fun)) {
            return "shape=circle";
        } else
            return "shape=Mrecord";
    }

    template<class EdgeIter>
    static std::string getEdgeAttributes(PTACallGraphNode *node, EdgeIter EI, PTACallGraph *PTACallGraph) {

        //TODO: mark indirect call of Fork with different color
        PTACallGraphEdge* edge = *(EI.getCurrent());
        assert(edge && "No edge found!!");

        std::string color;

        if (edge->getEdgeKind() == PTACallGraphEdge::TDJoinEdge) {
            color = "color=green";
        } else if (edge->getEdgeKind() == PTACallGraphEdge::TDForkEdge) {
            color = "color=blue";
        } else {
            color = "color=black";
        }
        if (0 != edge->getIndirectCalls().size()) {
            color = "color=red";
        }
        return color;
    }
};
}