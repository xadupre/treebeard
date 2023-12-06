#ifndef ONNX_MODEL_PARSER_H
#define ONNX_MODE_PARSER_H

#include "CompileUtils.h"
#include "DecisionForest.h"
#include "ExecutionHelpers.h"
#include "Representations.h"
#include "TreebeardContext.h"
#include "forestcreator.h"
#include <cstdint>
#include <ios>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <fstream>

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/MLIRContext.h"

// !!! "onnx.pb.h" is generated by protoc from onnx.proto.
// File is present here: https://github.com/onnx/onnx/blob/9b7bca2a723ff94edcd007d93b5d0cf1838591dc/onnx/onnx.proto
// command: protoc --proto_path=<path to directory containing onnx.proto> --cpp_out=<output director> onnx.proto
#include "onnx.pb.h"

namespace TreeBeard
{
    using ONNXModelParseResult = struct OnnxModelParseResult {
    float baseValue;
    mlir::decisionforest::PredictionTransformation predTransform;
    int64_t numNodes;
    int64_t numTargets;
    const int64_t *treeIds;
    const int64_t *nodeIds;
    const int64_t *featureIds;
    const float *thresholds;
    const int64_t *falseNodeIds;
    const int64_t *trueNodeIds;
    mlir::arith::CmpFPredicate nodeMode;
    int64_t numberOfClasses;
    const int64_t *targetClassTreeId;
    const int64_t *targetClassNodeId;
    const int64_t *targetClassIds;
    const float *targetWeights;
    int64_t numWeights;

    static struct OnnxModelParseResult parseModel(const std::string &modelPath) {
        std::ifstream input(modelPath, std::ios::ate | std::ios::binary);
        if (!input.is_open()) {
        std::cout << "Error opening file: " << modelPath << std::endl;
        }

        input.seekg(0, std::ios::end);
        std::streamsize size = input.tellg();
        input.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);

        if (!input.read(buffer.data(), size)) {
        std::cout << "Error reading file: " << modelPath << std::endl;
        }

        struct OnnxModelParseResult result;
        result.model.ParseFromArray(buffer.data(), size);

        const auto &graph = result.model.graph();
        assert(graph.node_size() == 1 && "Only one node is supported");

        // std::cout << "Node name: " << graph.node(0).name() << std::endl;
        // std::cout << "Op type: " << graph.node(0).op_type() << std::endl;
        const auto &treeNode = graph.node(0);
        assert(
            (treeNode.op_type() == "TreeEnsembleRegressor" ||
            treeNode.op_type() == "TreeEnsembleClassifier") &&
            "Only TreeEnsembleRegressor and TreeEnsembleClassifier are supported");
        result.isEnsembleClassifier =
            treeNode.op_type() == "TreeEnsembleClassifier";

        result.parseAttributes(treeNode);
        return result;
    }

    private:
    OnnxModelParseResult() = default;
    onnx::ModelProto model;
    bool isEnsembleClassifier = false;

    void parseAttributes(const onnx::NodeProto &node) {
        auto numberOfAttributes = node.attribute_size();
        for (int i = 0; i < numberOfAttributes; i++) {
        const auto &attribute = node.attribute(i);
        if (attribute.name() == "base_values") {
            assert(attribute.floats_size() == 1 &&
                "Only one base value is supported");
            baseValue = attribute.floats().data()[0];
        } else if (attribute.name() == "post_transform") {
            if (attribute.s() == "NONE") {
            predTransform =
                mlir::decisionforest::PredictionTransformation::kIdentity;
            } else if (attribute.s() == "SOFTMAX") {
            predTransform =
                mlir::decisionforest::PredictionTransformation::kSoftMax;
            } else if (attribute.s() == "LOGISTIC") {
            predTransform =
                mlir::decisionforest::PredictionTransformation::kSigmoid;
            } else {
            assert(false && "Unknown post_transform");
            }
        } else if (attribute.name() == "nodes_falsenodeids") {
            falseNodeIds = attribute.ints().data();
        } else if (attribute.name() == "nodes_truenodeids") {
            trueNodeIds = attribute.ints().data();
        } else if (attribute.name() == "nodes_featureids") {
            featureIds = attribute.ints().data();
        } else if (attribute.name() == "nodes_missing_value_tracks_true") {
            auto size = attribute.ints_size();
            auto data = attribute.ints().data();
            for (int i = 0; i < size; i++) {
            assert(data[i] == 0 && "Missing tracks are not supported");
            }
        } else if (attribute.name() == "nodes_modes") {
            auto size = attribute.strings_size();
            auto data = attribute.strings().data();
            if (size > 0) {
                auto first = *data[0];
                assert((first == "BRANCH_LT" || first == "BRANCH_GEQ" || first == "BRANCH_GT" || first == "BRANCH_LEQ") && "Only BRANCH_{LT, GEQ, GT, LEQ} is supported");
                for (int i = 1; i < size; i++) {
                    assert((*data[i] == first || *data[i] == "LEAF") &&
                        "Only BRANCH_{LT, GEQ, GT, LEQ} is supported");
                }
                if (first == "BRANCH_LT") {
                    nodeMode = mlir::arith::CmpFPredicate::ULT;
                } else if (first == "BRANCH_GEQ") {
                    nodeMode = mlir::arith::CmpFPredicate::UGE;
                } else if (first == "BRANCH_GT") {
                    nodeMode = mlir::arith::CmpFPredicate::UGT;
                } else if (first == "BRANCH_LEQ") {
                    nodeMode = mlir::arith::CmpFPredicate::ULE;
                }
                else {
                    // Default to BRANCH_LT
                    nodeMode = mlir::arith::CmpFPredicate::ULT;
                }
            }
        } else if (attribute.name() == "nodes_nodeids") {
            nodeIds = attribute.ints().data();
            numNodes = attribute.ints_size();
        } else if (attribute.name() == "nodes_treeids") {
            treeIds = attribute.ints().data();
        } else if (attribute.name() == "nodes_values") {
            thresholds = attribute.floats().data();
        } else if (attribute.name() == "target_ids") {
            targetClassIds = attribute.ints().data();
        } else if (attribute.name() == "target_nodeids") {
            targetClassNodeId = attribute.ints().data();
        } else if (attribute.name() == "target_treeids") {
            targetClassTreeId = attribute.ints().data();
        } else if (attribute.name() == "target_weights") {
            targetWeights = attribute.floats().data();
            numWeights = attribute.floats_size();
        } else if (attribute.name() == "n_targets") {
            numberOfClasses = isEnsembleClassifier ? attribute.i() : 0;
        } else {
            std::cout << "Unknown attribute : " << attribute.name() << std::endl;
        }
        }
    }
    };


    using ONNXTreeNodeKey = struct ONNXTreeKey {
        int64_t treeId;
        int64_t nodeId;
        
        bool operator==(const struct ONNXTreeKey &right) const {
          return (treeId == right.treeId) && (nodeId == right.nodeId);
        }

        bool operator<(const struct ONNXTreeKey &right) const {
          return ((treeId < right.treeId) || (treeId == right.treeId && nodeId < right.nodeId));
        }

        struct HashFn {
          std::size_t operator()(const struct ONNXTreeKey &key) const {
            std::size_t h1 = std::hash<int>()(key.treeId);
            std::size_t h2 = std::hash<int>()(key.nodeId);
            return h1 ^ h2;
          }
        };
    };

    template<typename ThresholdType>
    struct _ONNXTreeNode {
        int64_t featureId;
        ThresholdType threshold;
        std::shared_ptr<struct _ONNXTreeNode> leftChild = nullptr;
        std::shared_ptr<struct _ONNXTreeNode> rightChild = nullptr;
    };

    template<typename ThresholdType>
    using ONNXTreeNode = struct _ONNXTreeNode<ThresholdType>;

    template<typename ValueType>
    class ONNXModelConverter : public ForestCreator
    {
        private:
            std::vector<std::shared_ptr<ONNXTreeNode<ValueType>>> _trees;
            std::unordered_map<int64_t, std::set<int64_t>> _treeToClassIdMap;
            bool _isClassifier;
            std::vector<int64_t> _classIds;
            int32_t numberOfFeatures = -1;

        public:
            ONNXModelConverter(std::shared_ptr<mlir::decisionforest::IModelSerializer> serializer,
                            mlir::MLIRContext& context,
                            int32_t numFeatures,
                            ValueType baseValue,
                            mlir::decisionforest::PredictionTransformation predTransform,
                            mlir::arith::CmpFPredicate nodeMode,
                            int64_t numNodes,
                            const int64_t *treeIds,
                            const int64_t *nodeIds,
                            const int64_t *featureIds,
                            const ValueType *thresholds,
                            const int64_t *leftChildIds,
                            const int64_t *rightChildIds,
                            int64_t numberOfClasess,
                            const int64_t *targetClassTreeId,
                            const int64_t *targetClassNodeId,
                            const int64_t *targetClassIds,
                            const float *targetWeights,
                            int64_t numWeights,
                            int64_t batchSize
                            ) : ForestCreator(serializer,
                                                context,
                                                batchSize,
                                                // #TODOSampath - Revisit the types
                                                GetMLIRType(ValueType(), context),
                                                GetMLIRType(int32_t(), context),
                                                GetMLIRType(int32_t(), context),
                                                GetMLIRType(ValueType(), context),
                                                GetMLIRType(ValueType(), context)),
                                _isClassifier(false)
            {
                std::unordered_map<ONNXTreeNodeKey, std::shared_ptr<ONNXTreeNode<ValueType>>, ONNXTreeNodeKey::HashFn> nodeMap;

                assert(numFeatures > 0 && "Number of features should be > 0");
                this->numberOfFeatures = numFeatures;

                for (int64_t i = 0; i < numNodes; i++) {
                    ONNXTreeNodeKey key = {treeIds[i], nodeIds[i]};
                    auto onnxTreeNode = std::make_shared<ONNXTreeNode<ValueType>>();
                    onnxTreeNode->featureId = featureIds[i];
                    onnxTreeNode->threshold = thresholds[i];

                    nodeMap[key] = onnxTreeNode;
                }

                for (int64_t i = 0; i < numNodes; i++) {
                    ONNXTreeNodeKey key = {treeIds[i], nodeIds[i]};
                    auto node = nodeMap[key];

                    ONNXTreeNodeKey rightChildKey = {key.treeId, rightChildIds[i]};
                    auto rightChild = nodeMap.find(rightChildKey);

                    if (rightChild != nodeMap.end()) {
                        if (rightChildIds[i] > 0 && rightChildIds[i] < numNodes)
                            node->rightChild = rightChild->second;
                        else
                            node->rightChild = nullptr;
                    }
                    else {
                        assert(false && "Right child not found");
                    }

                    ONNXTreeNodeKey leftChildKey = {key.treeId, leftChildIds[i]};
                    auto leftChild = nodeMap.find(leftChildKey);
                    if (leftChild != nodeMap.end()) {
                        if (leftChildIds[i] > 0 && leftChildIds[i] < numNodes)
                            node->leftChild = leftChild->second;
                        else
                            node->leftChild = nullptr;
                    }
                    else {
                        assert(false && "Left child not found");
                    }
                }

                if (numberOfClasess > 0) _isClassifier = true;

                this->SetInitialOffset(baseValue);
                this->SetNumberOfClasses(numberOfClasess);
                this->m_forest->SetPredictionTransformation(predTransform);
                this->SetPredicateType(nodeMode);

                // #TODOSampath - Revisit this hardcoding.
                this->SetReductionType(mlir::decisionforest::ReductionType::kAdd);

                int64_t currTreeId = -1;
                for (int64_t i = 0; i < numNodes; i++) {
                    if (currTreeId == -1 || treeIds[i] != currTreeId) {
                        _trees.push_back(nodeMap[{treeIds[i], nodeIds[i]}]);
                    }
                    currTreeId = treeIds[i];
                }

                // ONNX uses weights instead of threshould of leaves to compute final prediction. Treebeard uses leaf threshold value.
                // setting threshld value of leaves to weights to compute final prediction.
                // ONNX also uses different class IDs for each leaf node which treebeard currently doesn't support. We are just storing the data for now.
                for (int64_t i = 0; i < numWeights; i++) {
                    ONNXTreeKey key = {targetClassTreeId[i], targetClassNodeId[i]};
                    auto node = nodeMap.find(key);

                    if (node != nodeMap.end()) {
                        _treeToClassIdMap[targetClassTreeId[i]].insert(targetClassIds[i]);
                        node->second->threshold = targetWeights[i];
                    }
                    else {
                        assert(false && "Key not found");
                    }
                }

                for (int32_t featureId = 0; featureId < this->numberOfFeatures; featureId++) {
                    this->AddFeature(std::to_string(featureId), sizeof(ValueType) == sizeof(double) ? "double" : "float");
                }
            }

            int64_t constructSingleTree(const std::shared_ptr<ONNXTreeNode<ValueType>>& parent)
            {
                if (parent) {
                    auto rootIndex = this->NewNode(parent->threshold, parent->featureId);
                    auto leftChildIndex = constructSingleTree(parent->leftChild);
                    if (leftChildIndex != -1) {
                        this->SetNodeLeftChild(rootIndex, leftChildIndex);
                        this->SetNodeParent(leftChildIndex, rootIndex);
                    }
                    auto rightChildIndex = constructSingleTree(parent->rightChild);
                    if (rightChildIndex != -1) {
                        this->SetNodeRightChild(rootIndex, rightChildIndex);
                        this->SetNodeParent(rightChildIndex, rootIndex);
                    }

                    return rootIndex;
                }

                return -1;
            }   

            void ConstructForest() override
            {
                int64_t treeIdIndex = 0;
                for (const auto& tree : _trees) {
                    this->NewTree();
                    auto rootIndex = constructSingleTree(tree);

                    // Root has no parent, setting to -1
                    this->SetNodeParent(rootIndex, -1);

                    if (_isClassifier) {
                        assert(_treeToClassIdMap[treeIdIndex].size() == 1 && "ONNX classifier with multiple class IDs per tree is not supported");
                        this->SetTreeClassId(*_treeToClassIdMap[treeIdIndex].begin());
                    }
                    
                    this->EndTree();
                    treeIdIndex++;
                }
            }
    };

    template<typename ValueType>
    class ONNXFileParser : public ForestCreator
    {
        private:
            std::unique_ptr<ONNXModelConverter<ValueType>> m_modelConvertor = nullptr;
            int32_t numFeatures = -1;
        public:
            ONNXFileParser(TreebeardContext& tbContext) : ForestCreator(tbContext.serializer,
                                             tbContext.context,
                                             tbContext.options.batchSize,
                                             // #TODOSampath - Revisit the types
                                             GetMLIRType(ValueType(), tbContext.context),
                                             GetMLIRType(int32_t(), tbContext.context),
                                             GetMLIRType(int32_t(), tbContext.context),
                                             GetMLIRType(ValueType(), tbContext.context),
                                             GetMLIRType(ValueType(), tbContext.context))
            {
                mlir::MLIRContext& context = tbContext.context;
                assert(tbContext.options.numberOfFeatures > 0 && "Number of features must be greater than 0");
                numFeatures = tbContext.options.numberOfFeatures;
                const auto &parsedModel = TreeBeard::ONNXModelParseResult::parseModel(tbContext.modelPath);

                // Hardcoding to float because ONNX doesn't support double. Revisit this #TODOSampath
                m_modelConvertor = std::make_unique<ONNXModelConverter<ValueType>>(
                    tbContext.serializer, context, this->numFeatures, parsedModel.baseValue,
                    parsedModel.predTransform, parsedModel.nodeMode, parsedModel.numNodes, parsedModel.treeIds,
                    parsedModel.nodeIds, parsedModel.featureIds, parsedModel.thresholds,
                    parsedModel.trueNodeIds, parsedModel.falseNodeIds,
                    parsedModel.numberOfClasses, parsedModel.targetClassTreeId,
                    parsedModel.targetClassNodeId, parsedModel.targetClassIds,
                    parsedModel.targetWeights, parsedModel.numWeights, tbContext.options.batchSize);
                
                m_modelConvertor->ConstructForest();
            }

            void ConstructForest() override
            {
                *this->m_forest = *m_modelConvertor->GetForest();
            }
    };

    template <typename T>
    mlir::decisionforest::InferenceRunnerBase*
    CreateInferenceRunnerForONNXModel(const char *modelPath,
                                  const char *modelGlobalsJSONPath,
                                  CompilerOptions *optionsPtr) {

      TreeBeard::TreebeardContext tbContext(modelPath, 
                                            modelGlobalsJSONPath,
                                            *optionsPtr,
                                            mlir::decisionforest::ConstructRepresentation(),
                                            mlir::decisionforest::ConstructModelSerializer(modelGlobalsJSONPath),
                                            nullptr /*TODO_ForestCreator*/ );
      // Hardcoding to float because ONNX doesn't support double. Revisit this
      // #TODOSampath
      ONNXFileParser<T> onnxModelParser(tbContext);
      mlir::ModuleOp module = TreeBeard::ConstructLLVMDialectModuleFromForestCreator(tbContext, onnxModelParser);

      auto *inferenceRunner = new mlir::decisionforest::InferenceRunner(
          tbContext.serializer, module, optionsPtr->tileSize,
          optionsPtr->thresholdTypeWidth, optionsPtr->featureIndexTypeWidth);
      return inferenceRunner;
    }

    inline std::shared_ptr<ForestCreator> ConstructONNXFileParser(TreebeardContext& tbContext) {
        return std::make_shared<ONNXFileParser<float>>(tbContext);
    }
}

#endif // ONNX_MODEL_PARSER_H

