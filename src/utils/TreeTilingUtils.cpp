#include <queue>
#include "TreeTilingDescriptor.h"
#include "TreeTilingUtils.h"
#include "TiledTree.h"
#include "llvm/ADT/STLExtras.h"

namespace mlir
{
namespace decisionforest
{

ForestJSONReader ForestJSONReader::m_instance;

// TODO How does this work for the last tree? We need to also know the length of the full serialization to compute the length
// of the last tree
int32_t ForestJSONReader::GetLengthOfTree(std::vector<int32_t>& offsets, int32_t treeIndex) {
  auto startOffset = offsets[treeIndex];
  ++treeIndex;
  while (offsets[treeIndex] == -1)
    ++treeIndex;
  auto endOffset = offsets[treeIndex];
  return endOffset - startOffset;
}

// Since this a tree compile time function, we don't really care about the actual 
// type of threshold and index. We'll just write the widths to the file and make
// sure we initialize the runtime buffer (model memrefs) with the correct types
void ForestJSONBuilder::AddTreesToJSON(std::list<int32_t>& treeIndices, std::list<std::vector<ThresholdType>>& serializedThresholds,
                                       std::list<std::vector<FeatureIndexType>>& serializedFetureIndices,
                                       const int32_t tileSize, const int32_t thresholdBitWidth, const int32_t indexBitWidth) {
    json treeSet;
    treeSet["tileSize"] = tileSize;
    treeSet["thresholdBitWidth"] = thresholdBitWidth;
    treeSet["indexBitWidth"] = indexBitWidth;

    // list of lists that represent the node values
    json treeValues;
    assert (serializedThresholds.size() == serializedFetureIndices.size());
    assert (treeIndices.size() == serializedFetureIndices.size());
    auto thresholdIter = serializedThresholds.begin();
    auto featureIndexIter = serializedFetureIndices.begin();
    auto treeIndexIter = treeIndices.begin();
    while(thresholdIter != serializedThresholds.end()) {
        json singleTree;
        auto& thresholds = *thresholdIter;
        auto& featureIndices = *featureIndexIter;
        assert (thresholds.size() == featureIndices.size());
        singleTree["treeIndex"] = *treeIndexIter;
        singleTree["numberOfNodes"] = thresholds.size();

        json nodeVals;
        for (size_t i=0 ; i<thresholds.size() ; ++i) {
            json node;
            node["threshold"] = thresholds[i];
            node["featureIndex"] = featureIndices[i];
            nodeVals.push_back(node);
        }
        singleTree["nodes"] = nodeVals;
        treeValues.push_back(singleTree);

        ++thresholdIter;
        ++featureIndexIter;
        ++treeIndexIter;
    }
    treeSet["trees"] = treeValues;
    m_json.push_back(treeSet);
}

void ForestJSONReader::ParseJSONFile() {
    assert (false && "Function not implemented!");
}

template<typename T>
void AppendAtEndOfList(std::list<T>& l, std::list<T>& newElements) {
    l.insert(std::end(l), std::begin(newElements), std::end(newElements));
}

void ForestJSONReader::AddSingleTileSizeEntry(std::list<int32_t>& treeIndices, std::list<int32_t>& numTilesList, std::list<std::vector<ThresholdType>>& serializedThresholds, 
                                              std::list<std::vector<FeatureIndexType>>& serializedFetureIndices,
                                              const int32_t tileSize, const int32_t thresholdBitWidth, const int32_t indexBitWidth) {
    // Find if there is already an entry with the given tileSize, thresholdWidth and indexWidth.
    auto listIter = this->m_tileSizeEntries.begin();
    while (listIter != m_tileSizeEntries.end()) {
        if (listIter->tileSize == tileSize && listIter->thresholdBitWidth==thresholdBitWidth && listIter->indexBitWidth==indexBitWidth)
            break;
        ++listIter;
    }
    if (listIter == m_tileSizeEntries.end()) {
        SingleTileSizeEntry entry {tileSize, thresholdBitWidth, indexBitWidth, treeIndices, numTilesList, serializedThresholds, serializedFetureIndices};
        m_tileSizeEntries.push_back(entry);
    }
    else {
        AppendAtEndOfList(listIter->treeIndices, treeIndices);
        AppendAtEndOfList(listIter->numberOfTiles, numTilesList);
        AppendAtEndOfList(listIter->serializedThresholds, serializedThresholds);
        AppendAtEndOfList(listIter->serializedFetureIndices, serializedFetureIndices);
    }
}

void ForestJSONReader::AddSingleTree(int32_t treeIndex, int32_t numTiles, std::vector<ThresholdType>& serializedThresholds, std::vector<FeatureIndexType>& serializedFetureIndices,
                                     const int32_t tileSize, const int32_t thresholdBitWidth, const int32_t indexBitWidth) {
    std::list<int32_t> treeIndices = { treeIndex };
    std::list<int32_t> numTilesList = { numTiles };
    std::list<std::vector<ThresholdType>> serializedThresholdsList = { serializedThresholds };
    std::list<std::vector<FeatureIndexType>> serializedFetureIndicesList = { serializedFetureIndices };

    AddSingleTileSizeEntry(treeIndices, numTilesList, serializedThresholdsList, serializedFetureIndicesList, tileSize, thresholdBitWidth, indexBitWidth);
}

void ForestJSONReader::ClearAllData() {
    m_tileSizeEntries.clear();
}

template<typename DestType, typename SourceType>
struct ElementCopier {
    void copyElement(char *buf, SourceType& val) {
        *reinterpret_cast<DestType*>(buf) = static_cast<DestType>(val);
    }

    void incrementPtr(char* &buf){
        buf += sizeof(DestType);
    }
};

class CopyModelValuesIntoBufferInterface {
    void IncrementPointer(char*& ptr, int32_t bytesToIncrement) {
        ptr += bytesToIncrement;
    }
public:
    virtual void CopyElements(char* bufPtr, std::vector<int32_t>& offsets, std::list<int32_t>& numberOfTiles, std::list<std::vector<ThresholdType>>& thresholdVals, 
                              std::list<std::vector<FeatureIndexType>>& featureIndices, std::list<int32_t>& treeIndices) = 0;
};

template<typename CopyThreshold, typename CopyFeatureIndex>
class CopyModelValuesIntoBuffer : public CopyModelValuesIntoBufferInterface {
    int32_t m_tileSize;
    // int32_t m_thresholdSizeInBytes;
    // int32_t m_featureIndexSizeInBytes;
    int32_t m_featureIndexStartOffsetInBytes;
    int32_t m_nodeSizeInBytes;
    CopyThreshold m_copyThreshold;
    CopyFeatureIndex m_copyFeatureIndex;

    int32_t CopySingleTree(char* &bufPtr, std::vector<ThresholdType>& thresholds, std::vector<FeatureIndexType>& featureIndices) {
        int32_t numTilesWritten = 0;
        assert (thresholds.size() == featureIndices.size());
        
        for (size_t i=0 ; i<thresholds.size() ; i+=m_tileSize) {
            // Copy a single tile
            // First the thresholds
            char *currPtr = bufPtr;
            for (size_t j=0 ; j<(size_t)m_tileSize ; ++j) {
                m_copyThreshold.copyElement(currPtr, thresholds[i+j]);
                m_copyThreshold.incrementPtr(currPtr);
            }
            // Then copy the feature indices
            currPtr = bufPtr + m_featureIndexStartOffsetInBytes;
            for (size_t j=0 ; j<(size_t)m_tileSize ; ++j) {
                m_copyFeatureIndex.copyElement(currPtr, featureIndices[i+j]);
                m_copyFeatureIndex.incrementPtr(currPtr);
            }
            bufPtr += m_nodeSizeInBytes;
            numTilesWritten += 1;
        }
        return numTilesWritten;
    }
public:
    CopyModelValuesIntoBuffer(int32_t tileSize, int32_t featureIndexStart, int32_t nodeSize)
        :m_tileSize(tileSize), m_featureIndexStartOffsetInBytes(featureIndexStart), m_nodeSizeInBytes(nodeSize)  
         // m_thresholdSizeInBytes(thresholdSize), m_featureIndexSizeInBytes(featureIndexSize)
    {}

    void CopyElements(char* bufPtr, std::vector<int32_t>& offsets, std::list<int32_t>& numberOfTiles, std::list<std::vector<ThresholdType>>& thresholdVals, 
                      std::list<std::vector<FeatureIndexType>>& featureIndices, std::list<int32_t>& treeIndices) override {
        
        // TODO this function assumes that all trees have non zero elements to write into the output buffer. 
        // This may not be the case when we have multiple tile sizes. This function needs to check and 
        // return early when a tree has no tiles to write for the current tile size (maybe offset[i] == offset[i+1]
        // -- but how will this work for the last tree?)
        // Actually, this is only going over tree indices that are non-empty. So maybe not a problem?
        assert (thresholdVals.size() == featureIndices.size());
        assert (treeIndices.size() == thresholdVals.size());

        auto thresholdIter = thresholdVals.begin();
        auto featureIndexIter = featureIndices.begin();
        auto treeIndexIter = treeIndices.begin();
        auto numTilesIter = numberOfTiles.begin();
        // This is the offset into the buffer in terms of tiles
        int32_t currTileOffset = 0;
        while (thresholdIter != thresholdVals.end())
        {
            // Write the offset of the current tree -- this is the start index (in tiles)
            // of the current tree. 
            auto treeIndex = *treeIndexIter;
            assert (offsets[treeIndex] == -1 && "Tree start offset can only be written once");
            offsets[treeIndex] = currTileOffset;

            // Copy the tiles of the current tree into the buffer
            auto tilesWritten = CopySingleTree(bufPtr, *thresholdIter, *featureIndexIter);

            currTileOffset += tilesWritten;
            assert(*numTilesIter == tilesWritten && "Number of tiles copied should match");
            ++thresholdIter;
            ++featureIndexIter;
            ++treeIndexIter;
            ++numTilesIter;
        }
    }
};

CopyModelValuesIntoBufferInterface* GetModelCopier(int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth){
    // TODO we need a better way to allocate these copiers. Maybe add a base class for the copiers so we can allocate them separately?
    
    // TODO We need to take care of alignment here. For now just copying things with 1 byte packing
    int32_t featureIndexStart = (tileSize * thresholdBitWidth/8); // Tile is written as <thresholds X tileSize, featureIndices X tileSize>
    int32_t tileSizeInBytes = ((thresholdBitWidth + indexBitWidth) * tileSize)/8;
    if (thresholdBitWidth == 32) {
        using ThresholdCopier = ElementCopier<float, ThresholdType>;
        if (indexBitWidth == 8)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int8_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else if(indexBitWidth == 16)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int16_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else if(indexBitWidth == 32)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int32_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else {
            assert (false && "unsupported feature index bitwidth");
            return nullptr;
        }

    }
    else if (thresholdBitWidth == 64) {
        using ThresholdCopier = ElementCopier<double, ThresholdType>;
        if (indexBitWidth == 8)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int8_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else if(indexBitWidth == 16)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int16_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else if(indexBitWidth == 32)
            return new CopyModelValuesIntoBuffer<ThresholdCopier, ElementCopier<int32_t, FeatureIndexType>>(tileSize, featureIndexStart, tileSizeInBytes);
        else {
            assert (false && "unsupported feature index bitwidth");
            return nullptr;
        }
    }
    return nullptr;
}

std::list<ForestJSONReader::SingleTileSizeEntry>::iterator ForestJSONReader::FindEntry(int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth) {
    // Find if there is already an entry with the given tileSize, thresholdWidth and indexWidth.
    auto listIter = this->m_tileSizeEntries.begin();
    while (listIter != m_tileSizeEntries.end()) {
        if (listIter->tileSize == tileSize && listIter->thresholdBitWidth==thresholdBitWidth && listIter->indexBitWidth==indexBitWidth)
            break;
        ++listIter;
    }
    assert (listIter != m_tileSizeEntries.end() && "Given tileSize and bit width entry must be present!");
    return listIter;
}

void ForestJSONReader::InitializeBuffer(void* bufPtr, int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth, std::vector<int32_t>& treeOffsets) {
    auto listIter = FindEntry(tileSize, thresholdBitWidth, indexBitWidth);
    auto modelCopier = GetModelCopier(tileSize, thresholdBitWidth, indexBitWidth);
    modelCopier->CopyElements(reinterpret_cast<char*>(bufPtr), treeOffsets, listIter->numberOfTiles, listIter->serializedThresholds, 
                              listIter->serializedFetureIndices, listIter->treeIndices);
}

void ForestJSONReader::InitializeOffsetBuffer(void* bufPtr, int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth) {
    IndexType *offsetBuffer = reinterpret_cast<IndexType*>(bufPtr);
    auto listIter = FindEntry(tileSize, thresholdBitWidth, indexBitWidth);
    assert (listIter->numberOfTiles.size() == listIter->treeIndices.size());
    auto currentOffset = 0;
    auto treeIndexIter = listIter->treeIndices.begin();
    std::vector<bool> treeIndexPresent(m_numberOfTrees, false);
    for (auto numTilesIter=listIter->numberOfTiles.begin() ; numTilesIter!=listIter->numberOfTiles.end() ; ++numTilesIter, ++treeIndexIter) {
        offsetBuffer[*treeIndexIter] = currentOffset;
        treeIndexPresent[*treeIndexIter] = true;
        currentOffset += *numTilesIter;
    }
    for (size_t index=0 ; index<treeIndexPresent.size() ; ++index) {
        if (treeIndexPresent[index] == false) {
            offsetBuffer[index] = -1;
        }
    }
}

void ForestJSONReader::InitializeLengthBuffer(void* bufPtr, int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth) {
    IndexType *lengthBuffer = reinterpret_cast<IndexType*>(bufPtr);
    auto listIter = FindEntry(tileSize, thresholdBitWidth, indexBitWidth);
    assert (listIter->numberOfTiles.size() == listIter->treeIndices.size());
    auto treeIndexIter = listIter->treeIndices.begin();
    std::vector<bool> treeIndexPresent(m_numberOfTrees, false);
    for (auto numTilesIter=listIter->numberOfTiles.begin() ; numTilesIter!=listIter->numberOfTiles.end() ; ++numTilesIter, ++treeIndexIter) {
        lengthBuffer[*treeIndexIter] = *numTilesIter;
        treeIndexPresent[*treeIndexIter] = true;
    }
    for (size_t index=0 ; index<treeIndexPresent.size() ; ++index) {
        if (treeIndexPresent[index] == false) {
            lengthBuffer[index] = 0;
        }
    }
}

// Ultimately, this will write a JSON file. For now, we're just 
// storing it in memory assuming the compiler and inference 
// will run in the same process. 
void PersistDecisionForest(mlir::decisionforest::DecisionForest<>& forest, mlir::decisionforest::TreeEnsembleType forestType) {
    mlir::decisionforest::ForestJSONReader::GetInstance().ClearAllData();

    auto numTrees = forest.NumTrees();
    mlir::decisionforest::ForestJSONReader::GetInstance().SetNumberOfTrees(numTrees);
    for (size_t i=0; i<numTrees ; ++i) {
        auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();
        
        // TODO We're assuming that the threshold type is a float type and index type 
        // is an integer. This is just to get the size. Can we get the size differently?
        auto thresholdType = treeType.getThresholdType().cast<FloatType>();
        auto featureIndexType = treeType.getFeatureIndexType().cast<IntegerType>(); 

        auto& tree = forest.GetTree(static_cast<int64_t>(i));
        std::vector<ThresholdType> thresholds = tree.GetThresholdArray();
        std::vector<FeatureIndexType> featureIndices = tree.GetFeatureIndexArray();
        int32_t numTiles = tree.GetNumberOfTiles();
        int32_t tileSize = tree.TilingDescriptor().MaxTileSize();
        
        mlir::decisionforest::ForestJSONReader::GetInstance().AddSingleTree(i, numTiles, thresholds, featureIndices, tileSize, 
                                                                            thresholdType.getWidth(), featureIndexType.getWidth());
    }
}

void ClearPersistedForest() {
    mlir::decisionforest::ForestJSONReader::GetInstance().ClearAllData();
}

// -----------------------------------------------
// Construction of Tile Tree
// -----------------------------------------------
bool TiledTreeNode::AreNodesInSameTile(int32_t node1, int32_t node2) {
    auto& tilingDescriptor = m_tiledTree.m_modifiedTree.TilingDescriptor();
    if(tilingDescriptor.TileIDs().at(node1) == tilingDescriptor.TileIDs().at(node2)) {
        assert (tilingDescriptor.TileIDs().at(node1) == m_tileID);
        return true;
    }
    return false;
}

int32_t TiledTreeNode::FindTileEntryNode() {
    int32_t entryNode = DecisionTree<>::INVALID_NODE_INDEX;
    for (auto nodeIdx : m_nodeIndices) {
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIdx);
        auto parentIdx = node.parent;
        // Figure out if the parent is in this tile
        bool isRoot = (parentIdx == DecisionTree<>::INVALID_NODE_INDEX);
        bool parentInTile = !isRoot && AreNodesInSameTile(parentIdx, nodeIdx);
        if (!parentInTile) {
            assert (entryNode == DecisionTree<>::INVALID_NODE_INDEX);
            entryNode = nodeIdx;
            // NOT breaking here so we check the rest of the nodes. We should never get back in here.
        }
    }
    assert (entryNode != DecisionTree<>::INVALID_NODE_INDEX);
    return entryNode;
}

void TiledTreeNode::SortTileNodes() {
    std::vector<int32_t> levelOrderSorted;
    levelOrderSorted.reserve(m_nodeIndices.size());
    std::queue<int32_t> traversalQ;
    auto entryNodeIndex = FindTileEntryNode();
    traversalQ.push(entryNodeIndex);
    while (!traversalQ.empty()) {
        int32_t nodeIndex = traversalQ.front();
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIndex);
        traversalQ.pop();
        assert (AreNodesInSameTile(entryNodeIndex, nodeIndex));
        levelOrderSorted.push_back(nodeIndex);
        // If the node is not a leaf, then it must have two valid children
        if (node.IsLeaf()) 
            continue;
        if (AreNodesInSameTile(node.leftChild, nodeIndex)) {
            assert (std::find(m_nodeIndices.begin(), m_nodeIndices.end(), node.leftChild) != m_nodeIndices.end());
            traversalQ.push(node.leftChild);
        }
        if (AreNodesInSameTile(node.rightChild, nodeIndex)) {
            assert (std::find(m_nodeIndices.begin(), m_nodeIndices.end(), node.rightChild) != m_nodeIndices.end());
            traversalQ.push(node.rightChild);
        }
    }
    assert(m_nodeIndices.size() == levelOrderSorted.size());
    m_nodeIndices = levelOrderSorted;
}

void TiledTreeNode::AddExtraNodesIfNeeded() {
    // How do we add the extra nodes in the right places in the vector? We need
    // to maintain level order!
    if (static_cast<int32_t>(m_nodeIndices.size()) == m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize())
        return;
    // If the only node in the tile is a leaf, then just return
    if (static_cast<int32_t>(m_nodeIndices.size()) == 1 && 
        m_tiledTree.m_modifiedTree.GetNodes().at(m_nodeIndices.at(0)).IsLeaf()) {
        return;
    }
    m_hasExtraNodes = true;
    assert (static_cast<int32_t>(m_nodeIndices.size()) < m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize());
    int32_t numberOfNodesToAdd = m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize() - static_cast<int32_t>(m_nodeIndices.size());
    // TODO Must there be at least one node where both children are leaves?
    // This must be true
    // 1. The child of any node must either be in the same tile or must be a leaf (if it were not, the tile could have been larger)
    // 2. Since the tile can't grow indefinitely, #1 => there is at least one node with both children being leaves
    std::list<int32_t> candidateNodes;
    for (auto nodeIndex : m_nodeIndices) {
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIndex);
        auto leftChildIndex = node.leftChild;
        auto rightChildIndex = node.rightChild;
        auto& leftChild = m_tiledTree.m_modifiedTree.GetNodes().at(leftChildIndex);
        auto& rightChild = m_tiledTree.m_modifiedTree.GetNodes().at(rightChildIndex);
        assert (AreNodesInSameTile(nodeIndex, leftChildIndex) || leftChild.IsLeaf());
        assert (AreNodesInSameTile(nodeIndex, rightChildIndex) || rightChild.IsLeaf());
        if (leftChild.IsLeaf() && rightChild.IsLeaf())
            candidateNodes.push_front(nodeIndex);
    }
    assert (candidateNodes.size() > 0);
    // TODO How do we determine the shape of this tile once we add new nodes? Maybe some kind of look up based on the positions of the nodes in the 
    // full dense serialization?
    // TODO How do we decide which of the candidate nodes to use as the parent of the new node(s)? For now, picking from the first candidate, which will 
    // be the right most node on the bottom most level. 
    auto candidateIter = candidateNodes.begin();
    for (int32_t i=0 ; i<numberOfNodesToAdd ; i+=2) { // We can add two dummy nodes for every candidate node
        // TODO How do we decide where to add the new nodes? Maybe just add them somewhere and call sort again?
        auto candidateIndex = *candidateIter;
        auto& candidateNode = m_tiledTree.m_modifiedTree.GetNodes().at(candidateIndex);
        {
          auto leafIndex = candidateNode.rightChild;
          auto &leafNode = m_tiledTree.m_modifiedTree.GetNodes().at(leafIndex);
          assert(leafNode.IsLeaf());
          // Add the dummy node as the right child of the candidate
          auto dummyNode = m_tiledTree.m_modifiedTree.NewNode(candidateNode.threshold, candidateNode.featureIndex, m_tileID);
          
          m_tiledTree.m_modifiedTree.SetNodeLeftChild(dummyNode, leafIndex);
          m_tiledTree.m_modifiedTree.SetNodeRightChild(dummyNode, leafIndex);
          m_tiledTree.m_modifiedTree.SetNodeParent(leafIndex, dummyNode);
          
          m_tiledTree.m_modifiedTree.SetNodeParent(dummyNode, candidateIndex);
          m_tiledTree.m_modifiedTree.SetNodeRightChild(candidateIndex, dummyNode);

          m_tiledTree.AddNodeToTile(m_tileIndex, dummyNode);
        }
        if (i+1 == numberOfNodesToAdd)
            break;
        {
          auto leafIndex = candidateNode.leftChild;
          auto &leafNode = m_tiledTree.m_modifiedTree.GetNodes().at(leafIndex);
          assert(leafNode.IsLeaf());
          // Add the dummy node as the left child of the candidate
          auto dummyNode = m_tiledTree.m_modifiedTree.NewNode(candidateNode.threshold, candidateNode.featureIndex, m_tileID);
          m_tiledTree.m_modifiedTree.SetNodeLeftChild(dummyNode, leafIndex);
          m_tiledTree.m_modifiedTree.SetNodeRightChild(dummyNode, leafIndex);
          m_tiledTree.m_modifiedTree.SetNodeParent(leafIndex, dummyNode);

          m_tiledTree.m_modifiedTree.SetNodeParent(dummyNode, candidateIndex);
          m_tiledTree.m_modifiedTree.SetNodeLeftChild(candidateIndex, dummyNode);
          
          m_tiledTree.AddNodeToTile(m_tileIndex, dummyNode);
        }
        ++candidateIter;
    }
    SortTileNodes();
    m_tiledTree.SetChildrenForTile(*this);
}

TiledTree::TiledTree(DecisionTree<>& owningTree)
 : m_owningTree(owningTree), m_modifiedTree(owningTree)
{
    ConstructTiledTree();
}

void TiledTree::SetChildrenHelper(TiledTreeNode& tile, int32_t nodeIndex, std::vector<int32_t>& children) {
    auto &node = m_modifiedTree.GetNodes().at(nodeIndex);
    if (node.IsLeaf()) return;
    if (tile.AreNodesInSameTile(nodeIndex, node.leftChild))
        SetChildrenHelper(tile, node.leftChild, children);
    else
        children.push_back(GetNodeTileIndex(node.leftChild));
    if (tile.AreNodesInSameTile(nodeIndex, node.rightChild))
        SetChildrenHelper(tile, node.rightChild, children);
    else
        children.push_back(GetNodeTileIndex(node.rightChild));
}

void TiledTree::SetChildrenForTile(TiledTreeNode& tile) {
    tile.m_children.clear();
    SetChildrenHelper(tile, tile.GetEntryNode(), tile.m_children);
}

void TiledTree::ConstructTiledTree() {
    const TreeTilingDescriptor& tilingDescriptor = m_modifiedTree.TilingDescriptor();
    auto& tileIDs = tilingDescriptor.TileIDs();
    std::map<int32_t, int32_t> tileIDToTileIndexMap;
    assert(m_modifiedTree.GetNodes().size() == tileIDs.size());
    // First, split the nodes among tiles
    int32_t nodeIndex=0;
    for (auto tileID : tileIDs) {
        auto tileIDMapIter = tileIDToTileIndexMap.find(tileID);
        int32_t tileIndex = -1;
        if (tileIDMapIter == tileIDToTileIndexMap.end()) {
            tileIndex = NewTiledTreeNode(tileID);
            tileIDToTileIndexMap[tileID] = tileIndex;
        }
        else {
            tileIndex = tileIDMapIter->second;
        }
        AddNodeToTile(tileIndex, nodeIndex);
        ++nodeIndex;
    }
    // Sort the nodes in each tile
    for (auto& tile : m_tiles)
        tile.SortTileNodes();
    // Set the parents and the children of each tile
    for (auto& tile : m_tiles) {
        auto entryNode = tile.GetEntryNode();
        auto parentNode = m_modifiedTree.GetNodes().at(entryNode).parent;
        auto parentTileIndex = GetNodeTileIndex(parentNode);
        tile.SetParent(parentTileIndex);

        SetChildrenForTile(tile);
    }
    // Expand the tiles that aren't full with dummy nodes (TODO how do you represent this?)
    for (auto& tile : m_tiles)
        tile.AddExtraNodesIfNeeded();
    
    assert(Validate());
}

bool TiledTree::Validate() {
    // Checks
    //  - All nodes in the owning tree are covered and contained in exactly one tile
    //  - All tiles are the same size or are leaves (with size == 1)
    //  - Leaves are not part of tiles
    //  - TODO Tiles are connected (traverse from entry tile and assert all nodes are reachable)
    std::vector<int32_t> nodeCounts(m_modifiedTree.GetNodes().size(), 0);
    for (auto& tile: m_tiles) {
        auto& tileNodeIndices = tile.GetNodeIndices();
        for (auto nodeIndex : tileNodeIndices) {
            if (nodeIndex >= static_cast<int32_t>(nodeCounts.size()))
                continue;
            nodeCounts.at(nodeIndex)++;
        }
        int32_t maxTileSize = m_modifiedTree.TilingDescriptor().MaxTileSize();
        if (tileNodeIndices.size() == 1) {
            if (!m_modifiedTree.GetNodes().at(tileNodeIndices.front()).IsLeaf()) {
                assert (false && "Node in tile with a single node must be a leaf");
                return false;
            }
        }
        else {
            if (static_cast<int32_t>(tileNodeIndices.size()) != maxTileSize) {
                assert (false && "Tile sizes must be equal except for leaf nodes");
                return false;
            }
            for (auto nodeIndex : tileNodeIndices) {
                // A node that is a non-unit tile must not be a leaf
                if (m_modifiedTree.GetNodes().at(nodeIndex).IsLeaf()) {
                    assert(false && "A node that is a non-unit tile must not be a leaf");
                    return false;
                }
            }
        }
    }
    for (auto nodeCount : nodeCounts) {
        if (nodeCount!=1) {
            assert (false && "Node must be in exactly one tile");
            return false;
        }
    }
    return true;
}

void TiledTreeNode::WriteDOTSubGraph(std::ofstream& fout) {
    std::vector<std::string> colors = { "aquamarine3", "darkolivegreen4", "deepskyblue", "firebrick", "grey80", "teal"};
    std::string& color = colors[m_tileID % colors.size()];

    fout << "subgraph tile_" << m_tileID << " {\n";
    fout << "\tnode [style=filled, ";
    fout << "color=" << color << "];\n";
    for (size_t i=0 ; i<m_nodeIndices.size() ; ++i) {
        auto nodeIndex = m_nodeIndices.at(i);
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIndex);
        int64_t parentIndex = node.parent;
        auto& parentNode = m_tiledTree.m_modifiedTree.GetNodes().at(parentIndex);
        fout << "\t\"node" << nodeIndex << "\" [ label = \"Id:" << nodeIndex << ", Thres:" << node.threshold << ", FeatIdx:" << node.featureIndex << "\"];\n";
        if (parentIndex != DecisionTree<>::INVALID_NODE_INDEX) {
            std::string edgeColor = parentNode.leftChild == nodeIndex ? "green" : "red";
            if (parentNode.leftChild == parentNode.rightChild)
                edgeColor = "black";
            fout << "\t\"node" << parentIndex << "\" -> \"node" << nodeIndex << "\"[style=bold,color=" << edgeColor << "];\n";
        }
    }
    fout << "}\n";
}

void TiledTree::EmitNodeDOT(std::ofstream& fout, int32_t nodeIndex) {
    std::vector<std::string> colors = { "aquamarine3", "darkolivegreen4", "deepskyblue", "firebrick", "grey80", "teal"};

    auto& node = m_modifiedTree.GetNodes().at(nodeIndex);
    int32_t tileID = this->GetNodeTileIndex(nodeIndex);
    std::string& color = colors[tileID % colors.size()];

    int64_t parentIndex = node.parent;
    fout << "\t\"node" << nodeIndex << "\" [ label = \"Id:" << nodeIndex
        << ", Thres:" << node.threshold
        << ", FeatIdx:" << node.featureIndex << "\", style=bold, color=" << color << "];\n";
    if (parentIndex != decisionforest::DecisionTree<>::INVALID_NODE_INDEX) {
        auto& parentNode = m_modifiedTree.GetNodes().at(parentIndex);
        std::string edgeColor = parentNode.leftChild == nodeIndex ? "green" : "red";
        if (parentNode.leftChild == parentNode.rightChild)
            edgeColor = "black";
        fout << "\t\"node" << parentIndex << "\" -> \"node" << nodeIndex << "\"[style=bold,color=" << edgeColor << "];\n";
        // fout << "\t\"node" << parentIndex << "\" -> \"node" << nodeIndex << "\";\n";
    }
    if (node.leftChild != decisionforest::DecisionTree<>::INVALID_NODE_INDEX)
        EmitNodeDOT(fout, node.leftChild);
    if (node.rightChild != decisionforest::DecisionTree<>::INVALID_NODE_INDEX)
        EmitNodeDOT(fout, node.rightChild);
}

// Routines to output DOT files for the tiled tree
void TiledTree::WriteDOTFile(const std::string& filename) {
    std::ofstream fout(filename);
#ifdef EMIT_TILES_AS_SUBGRAPHS
    fout << "digraph {\n";
    for (size_t i=0 ; i<m_tiles.size() ; ++i) {
        m_tiles[i].WriteDOTSubGraph(fout);
    }
    fout << "}\n";
#else // EMIT_TILES_AS_SUBGRAPHS
    fout << "digraph {\n";
    // TODO This assumes the root is the first node
    EmitNodeDOT(fout, 0);
    fout << "}\n";

#endif // EMIT_TILES_AS_SUBGRAPHS
}

void TiledTreeNode::GetThresholds(std::vector<double>::iterator beginIter) {
    if (m_nodeIndices.size() == 1) {
        // This is a leaf tile
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(m_nodeIndices.front());
        assert (node.IsLeaf() && "A tile with a single node can only contain a leaf");
        int32_t tileSize = m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize();
        auto threshold = node.threshold;
        for (int32_t i=0; i<tileSize ; ++i) {
            *beginIter = threshold;
            ++beginIter;
        }
        return;
    }
    assert (static_cast<int32_t>(m_nodeIndices.size()) == m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize());
    for (auto nodeIndex : m_nodeIndices) {
        auto threshold = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIndex).threshold;
        *beginIter = threshold;
        ++beginIter;
    }
}

void TiledTreeNode::GetFeatureIndices(std::vector<int32_t>::iterator beginIter) {
    if (m_nodeIndices.size() == 1) {
        // This is a leaf tile
        auto& node = m_tiledTree.m_modifiedTree.GetNodes().at(m_nodeIndices.front());
        assert (node.IsLeaf() && "A tile with a single node can only contain a leaf");
        int32_t tileSize = m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize();
        auto featureIndex = node.featureIndex;
        for (int32_t i=0; i<tileSize ; ++i) {
            *beginIter = featureIndex;
            ++beginIter;
        }
        return;
    }
    assert (static_cast<int32_t>(m_nodeIndices.size()) == m_tiledTree.m_modifiedTree.TilingDescriptor().MaxTileSize());
    for (auto nodeIndex : m_nodeIndices) {
        auto featureIndex = m_tiledTree.m_modifiedTree.GetNodes().at(nodeIndex).featureIndex;
        *beginIter = featureIndex;
        ++beginIter;
    }
}

int32_t TiledTree::GetTreeDepthHelper(int32_t tileIndex) {
    auto& tile = m_tiles.at(tileIndex);
    int32_t depth = 0;
    for (auto child : tile.GetChildren()) {
        depth = std::max(depth, GetTreeDepthHelper(child));
    }
    return 1+depth;
}

template <typename AttribType, typename GetterType>
void TiledTree::GetTileAttributeArray(std::vector<AttribType>& attributeVec,
                                      size_t vecIndex, size_t tileIndex, GetterType get)
{
    auto& tile = m_tiles.at(tileIndex);
    assert(vecIndex < attributeVec.size());
    // TODO What is the type we set on leaf nodes?
    // assert(node.featureType == FeatureType::kNumerical || node.IsLeaf());
    auto tileSize = m_owningTree.TilingDescriptor().MaxTileSize();
    get(tile, attributeVec.begin() + vecIndex*tileSize);
    const auto& children = tile.GetChildren();
    int32_t numChildren = tileSize + 1;
    for (size_t i=0 ; i<children.size() ; ++i) {
        int32_t childIndex = children[i];
        GetTileAttributeArray<AttribType, GetterType>(attributeVec, numChildren*vecIndex+i+1, childIndex, get);
    }
}

std::vector<double> TiledTree::SerializeThresholds() {
    int32_t tiledTreeDepth = GetTreeDepth();
    int32_t numChildrenPerTile = m_owningTree.TilingDescriptor().MaxTileSize() + 1;
    int32_t numberOfTiles = (std::pow(numChildrenPerTile, tiledTreeDepth) - 1)/(numChildrenPerTile - 1);
    int32_t vectorLength = numberOfTiles * m_owningTree.TilingDescriptor().MaxTileSize();
    std::vector<double> thresholds(vectorLength, -1);
    GetTileAttributeArray(thresholds, 0, 0, [&](TiledTreeNode& t, std::vector<double>::iterator iter){ t.GetThresholds(iter); } );
    return thresholds;
}

std::vector<int32_t> TiledTree::SerializeFeatureIndices() {
    int32_t tiledTreeDepth = GetTreeDepth();
    int32_t numChildrenPerTile = m_owningTree.TilingDescriptor().MaxTileSize() + 1;
    int32_t numberOfTiles = (std::pow(numChildrenPerTile, tiledTreeDepth) - 1)/(numChildrenPerTile - 1);
    int32_t vectorLength = numberOfTiles * m_owningTree.TilingDescriptor().MaxTileSize();
    std::vector<int32_t> thresholds(vectorLength, -1);
    GetTileAttributeArray(thresholds, 0, 0, [&](TiledTreeNode& t, std::vector<int32_t>::iterator iter){ t.GetFeatureIndices(iter); } );
    return thresholds;
}

} // decisionforest
} // mlir