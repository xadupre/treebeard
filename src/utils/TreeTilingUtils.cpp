#include "TreeTilingDescriptor.h"
#include "TreeTilingUtils.h"

namespace mlir
{
namespace decisionforest
{

ForestJSONReader ForestJSONReader::m_instance;

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

void ForestJSONReader::AddSingleTileSizeEntry(std::list<int32_t>& treeIndices, std::list<std::vector<ThresholdType>>& serializedThresholds, 
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
        SingleTileSizeEntry entry {tileSize, thresholdBitWidth, indexBitWidth, treeIndices, serializedThresholds, serializedFetureIndices};
        m_tileSizeEntries.push_back(entry);
    }
    else {
        AppendAtEndOfList(listIter->treeIndices, treeIndices);
        AppendAtEndOfList(listIter->serializedThresholds, serializedThresholds);
        AppendAtEndOfList(listIter->serializedFetureIndices, serializedFetureIndices);
    }
}

void ForestJSONReader::AddSingleTree(int32_t treeIndex, std::vector<ThresholdType>& serializedThresholds, std::vector<FeatureIndexType>& serializedFetureIndices,
                                     const int32_t tileSize, const int32_t thresholdBitWidth, const int32_t indexBitWidth) {
    std::list<int32_t> treeIndices = { treeIndex };
    std::list<std::vector<ThresholdType>> serializedThresholdsList = { serializedThresholds };
    std::list<std::vector<FeatureIndexType>> serializedFetureIndicesList = { serializedFetureIndices };

    AddSingleTileSizeEntry(treeIndices, serializedThresholdsList, serializedFetureIndicesList, tileSize, thresholdBitWidth, indexBitWidth);
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
    virtual void CopyElements(char* bufPtr, std::vector<int32_t>& offsets, std::list<std::vector<ThresholdType>>& thresholdVals, 
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

    void CopyElements(char* bufPtr, std::vector<int32_t>& offsets, std::list<std::vector<ThresholdType>>& thresholdVals, 
                      std::list<std::vector<FeatureIndexType>>& featureIndices, std::list<int32_t>& treeIndices) override {
        
        assert (thresholdVals.size() == featureIndices.size());
        assert (treeIndices.size() == thresholdVals.size());

        auto thresholdIter = thresholdVals.begin();
        auto featureIndexIter = featureIndices.begin();
        auto treeIndexIter = treeIndices.begin();
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

            ++thresholdIter;
            ++featureIndexIter;
            ++treeIndexIter;
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

void ForestJSONReader::InitializeBuffer(void* bufPtr, int32_t tileSize, int32_t thresholdBitWidth, int32_t indexBitWidth, std::vector<int32_t>& treeOffsets) {
    // Find if there is already an entry with the given tileSize, thresholdWidth and indexWidth.
    auto listIter = this->m_tileSizeEntries.begin();
    while (listIter != m_tileSizeEntries.end()) {
        if (listIter->tileSize == tileSize && listIter->thresholdBitWidth==thresholdBitWidth && listIter->indexBitWidth==indexBitWidth)
            break;
        ++listIter;
    }
    assert (listIter != m_tileSizeEntries.end() && "Given tileSize and bit width entry must be present!");
    auto modelCopier = GetModelCopier(tileSize, thresholdBitWidth, indexBitWidth);
    modelCopier->CopyElements(reinterpret_cast<char*>(bufPtr), treeOffsets, listIter->serializedThresholds, listIter->serializedFetureIndices, listIter->treeIndices);
}

// Ultimately, this will write a JSON file. For now, we're just 
// storing it in memory assuming the compiler and inference 
// will run in the same process. 
void PersistDecisionForest(mlir::decisionforest::DecisionForest<>& forest, mlir::decisionforest::TreeEnsembleType forestType) {
    auto numTrees = forest.NumTrees();
    for (size_t i=0; i<numTrees ; ++i) {
        auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();
        
        // TODO We're assuming that the threshold type is a float type and index type 
        // is an integer. This is just to get the size. Can we get the size differently?
        auto thresholdType = treeType.getThresholdType().cast<FloatType>();
        auto featureIndexType = treeType.getFeatureIndexType().cast<IntegerType>(); 

        auto& tree = forest.GetTree(static_cast<int64_t>(i));
        std::vector<ThresholdType> thresholds = tree.GetThresholdArray();
        std::vector<FeatureIndexType> featureIndices = tree.GetFeatureIndexArray();
        int32_t tileSize = tree.TilingDescriptor().MaxTileSize();
        
        mlir::decisionforest::ForestJSONReader::GetInstance().AddSingleTree(i, thresholds, featureIndices, tileSize, thresholdType.getWidth(), featureIndexType.getWidth());
    }
}

void ClearPersistedForest() {
    mlir::decisionforest::ForestJSONReader::GetInstance().ClearAllData();
}


} // decisionforest
} // mlir