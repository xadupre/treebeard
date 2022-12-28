#include "Dialect.h"
#include "OpLoweringUtils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "GPURepresentations.h"
#include "LIRLoweringHelpers.h"

using namespace mlir;
using namespace mlir::decisionforest::helpers;

namespace mlir
{
namespace decisionforest
{

void GPUArrayBasedRepresentation::GenerateSimpleInitializer(const std::string& funcName, ConversionPatternRewriter &rewriter, Location location, 
                                                            ModuleOp module, MemRefType memrefType) {
  // TODO why did this not work when I used the rewriter instead of the builder?
  // auto insertPoint = rewriter.saveInsertionPoint();
  auto functionType = FunctionType::get(rewriter.getContext(), {memrefType}, {memrefType});
  NamedAttribute visibilityAttribute{module.getSymVisibilityAttrName(), rewriter.getStringAttr("public")};
  auto initFunc = func::FuncOp::create(location, funcName, functionType, ArrayRef<NamedAttribute>(visibilityAttribute));
  auto &entryBlock = *initFunc.addEntryBlock();
  // rewriter.setInsertionPointToStart(&entryBlock);
  mlir::OpBuilder builder(initFunc.getContext());
  builder.setInsertionPointToStart(&entryBlock);
  auto waitOp = builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  auto alloc = builder.create<gpu::AllocOp>(location, memrefType, waitOp.asyncToken().getType(), ValueRange{waitOp.asyncToken()}, ValueRange{}, ValueRange{});
  auto transfer = builder.create<gpu::MemcpyOp>(location, alloc.asyncToken().getType(), ValueRange{alloc.asyncToken()}, 
                                                      alloc.memref(), static_cast<Value>(initFunc.getArgument(0)));
  /*auto waitBeforeReturn =*/ builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{transfer.getAsyncToken()});
  builder.create<mlir::func::ReturnOp>(location, static_cast<Value>(alloc.memref()));
  module.push_back(initFunc);
  // rewriter.setInsertionPoint(insertPoint.getBlock(), insertPoint.getPoint());
}

void GPUArrayBasedRepresentation::GenerateModelMemrefInitializer(const std::string& funcName, ConversionPatternRewriter &rewriter, Location location, 
                                                                 ModuleOp module, MemRefType memrefType) {
  assert (memrefType.getShape().size() == 1);
  SaveAndRestoreInsertionPoint saveAndRestoreEntryPoint(rewriter);
  auto modelMemrefElementType = memrefType.getElementType().cast<decisionforest::TiledNumericalNodeType>();
  int32_t tileSize = modelMemrefElementType.getTileSize();
  auto thresholdArgType = MemRefType::get({ memrefType.getShape()[0] * tileSize }, modelMemrefElementType.getThresholdElementType());
  auto indexArgType = MemRefType::get({ memrefType.getShape()[0] * tileSize }, modelMemrefElementType.getIndexElementType());
  auto tileShapeIDArgType = MemRefType::get(memrefType.getShape(), modelMemrefElementType.getTileShapeType());
  auto initModelMemrefFuncType = rewriter.getFunctionType(TypeRange{thresholdArgType, indexArgType, tileShapeIDArgType}, memrefType);
  NamedAttribute visibilityAttribute{module.getSymVisibilityAttrName(), rewriter.getStringAttr("public")};
  auto initModelMemrefFunc = mlir::func::FuncOp::create(location, funcName, initModelMemrefFuncType, ArrayRef<NamedAttribute>(visibilityAttribute));
  auto &entryBlock = *initModelMemrefFunc.addEntryBlock();
  // rewriter.setInsertionPointToStart(&entryBlock);
  mlir::OpBuilder builder(initModelMemrefFunc.getContext());
  builder.setInsertionPointToStart(&entryBlock);

  // Allocate the model memref
  auto waitOp = builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  auto alloc = builder.create<gpu::AllocOp>(location, memrefType, waitOp.asyncToken().getType(), ValueRange{waitOp.asyncToken()}, ValueRange{}, ValueRange{});

  auto asyncTokenType = alloc.asyncToken().getType();
  // Allocate and transfer all the arguments
  auto allocThresholds = builder.create<gpu::AllocOp>(location, thresholdArgType, asyncTokenType, ValueRange{alloc.asyncToken()}, ValueRange{}, ValueRange{});
  auto transferThresholds = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocThresholds.asyncToken()}, 
                                                      allocThresholds.memref(), static_cast<Value>(initModelMemrefFunc.getArgument(0)));

  auto allocFeatureIndices = builder.create<gpu::AllocOp>(location, indexArgType, asyncTokenType, ValueRange{transferThresholds.asyncToken()}, ValueRange{}, ValueRange{});
  auto transferFeatureIndices = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocFeatureIndices.asyncToken()}, 
                                                      allocFeatureIndices.memref(), static_cast<Value>(initModelMemrefFunc.getArgument(1)));

  auto allocTileShapeIds = builder.create<gpu::AllocOp>(location, tileShapeIDArgType, asyncTokenType, ValueRange{transferFeatureIndices.asyncToken()}, 
                                                        ValueRange{}, ValueRange{});
  auto transferTileShapeIds = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocTileShapeIds.asyncToken()}, 
                                                      allocTileShapeIds.memref(), static_cast<Value>(initModelMemrefFunc.getArgument(2)));

  // Create the gpu.launch op
  auto oneIndexConst = builder.create<arith::ConstantIndexOp>(location, 1);
  int32_t numThreadsPerBlock = 32;
  int32_t numBlocks = std::ceil((double)memrefType.getShape()[0]/numThreadsPerBlock);
  auto numThreadBlocksConst = builder.create<arith::ConstantIndexOp>(location, numBlocks);
  auto gpuLaunch = builder.create<gpu::LaunchOp>(location, numThreadBlocksConst, oneIndexConst, oneIndexConst, numThreadBlocksConst, oneIndexConst, oneIndexConst,
                                                nullptr, asyncTokenType, transferTileShapeIds.asyncToken());

  builder.setInsertionPointToStart(&gpuLaunch.body().front());
  
  // Generate the body of the launch op
  // auto numThreadsPerBlockConst = builder.create<arith::ConstantIndexOp>(location, numThreadsPerBlock);
  auto memrefLengthConst = builder.create<arith::ConstantIndexOp>(location, memrefType.getShape()[0]);
  auto firstThreadNum = builder.create<arith::MulIOp>(location, gpuLaunch.blockSizeX(), gpuLaunch.getBlockIds().x);
  auto elementIndex = builder.create<arith::AddIOp>(location, firstThreadNum, gpuLaunch.getThreadIds().x);
  auto inBoundsCondition = builder.create<arith::CmpIOp>(location, arith::CmpIPredicate::slt, elementIndex, memrefLengthConst);
  auto ifInBounds = builder.create<scf::IfOp>(location, inBoundsCondition, false);
  {
    // Generate the initialization code
    auto ifInsertionPoint = ifInBounds.getThenBodyBuilder().saveInsertionPoint();
    rewriter.setInsertionPoint(ifInsertionPoint.getBlock(), ifInsertionPoint.getPoint());
    this->GenModelMemrefInitFunctionBody(memrefType, alloc.memref(), rewriter, location, elementIndex, 
                                         alloc.memref(), allocFeatureIndices.memref(), allocTileShapeIds.memref());
  }
  // Wait and return 
  builder.setInsertionPointAfter(gpuLaunch); 
  /*auto waitBeforeReturn =*/ builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  builder.create<mlir::func::ReturnOp>(location, static_cast<Value>(alloc.memref()));
  module.push_back(initModelMemrefFunc);
}

mlir::LogicalResult GPUArrayBasedRepresentation::GenerateModelGlobals(Operation *op, ArrayRef<Value> operands, ConversionPatternRewriter &rewriter,
                                                                      std::shared_ptr<decisionforest::IModelSerializer> m_serializer) {
  auto location = op->getLoc();
  // Generate a new function with the extra arguments that are needed
  auto ensembleConstOp = AssertOpIsOfType<decisionforest::EnsembleConstantOp>(op);
  auto module = op->getParentOfType<mlir::ModuleOp>();
  assert (module);
  auto func = op->getParentOfType<func::FuncOp>();
  assert (func);

  mlir::decisionforest::DecisionForestAttribute forestAttribute = ensembleConstOp.forest();
  mlir::decisionforest::DecisionForest<>& forest = forestAttribute.GetDecisionForest();
  auto forestType = ensembleConstOp.getResult().getType().cast<decisionforest::TreeEnsembleType>();
  assert (forestType.doAllTreesHaveSameTileSize()); // There is still an assumption here that all trees have the same tile size
  auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();

  auto thresholdType = treeType.getThresholdType();
  auto featureIndexType = treeType.getFeatureIndexType(); 
  auto tileSize = treeType.getTileSize();
  auto tileShapeType = treeType.getTileShapeType();
  auto childIndexType = treeType.getChildIndexType();

  Type modelMemrefElementType = decisionforest::TiledNumericalNodeType::get(thresholdType, featureIndexType, tileShapeType, 
                                                                            tileSize, childIndexType);

  m_serializer->Persist(forest, forestType);
  
  auto modelMemrefSize = decisionforest::GetTotalNumberOfTiles();
  auto modelMemrefType = MemRefType::get({modelMemrefSize}, modelMemrefElementType);
  func.insertArgument(func.getNumArguments(), modelMemrefType, mlir::DictionaryAttr(), location);
  m_modelMemrefArgIndex = func.getNumArguments() - 1;

  auto offsetSize = (int32_t)forest.NumTrees();
  auto offsetMemrefType = MemRefType::get({offsetSize}, rewriter.getIndexType());
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_offsetMemrefArgIndex = func.getNumArguments() - 1;

  // Add the length argument
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_lengthMemrefArgIndex = func.getNumArguments() - 1;

  // Add the class info argument
  auto classInfoSize = forest.IsMultiClassClassifier() ? offsetSize : 0;
  auto classInfoMemrefType = MemRefType::get({classInfoSize}, treeType.getResultType());
  func.insertArgument(func.getNumArguments(), classInfoMemrefType, mlir::DictionaryAttr(), location);
  m_classInfoMemrefArgIndex = func.getNumArguments() - 1;

  m_modelMemref = func.getArgument(m_modelMemrefArgIndex);

  GenerateSimpleInitializer("Init_Offsets", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_Lengths", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_ClassIds", rewriter, location, module, classInfoMemrefType);
  GenerateModelMemrefInitializer("Init_Model", rewriter, location, module, modelMemrefType);

  EnsembleConstantLoweringInfo info 
  {
    static_cast<Value>(m_modelMemref),
    static_cast<Value>(func.getArgument(m_offsetMemrefArgIndex)),
    static_cast<Value>(func.getArgument(m_lengthMemrefArgIndex)),
    static_cast<Value>(func.getArgument(m_classInfoMemrefArgIndex)),
    modelMemrefType,
    offsetMemrefType,
    offsetMemrefType,
    classInfoMemrefType,
  };
  ensembleConstantToMemrefsMap[op] = info;
  return mlir::success();
}

// mlir::Value GPUArrayBasedRepresentation::GetTreeMemref(mlir::Value treeValue) {
//   return m_modelMemref;
// }

// void GPUArrayBasedRepresentation::GenerateTreeMemref(mlir::ConversionPatternRewriter &rewriter, mlir::Operation *op, Value ensemble, Value treeIndex) {

// }

// mlir::Value GPUArrayBasedRepresentation::GenerateGetTreeClassId(mlir::ConversionPatternRewriter &rewriter, mlir::Operation *op, Value ensemble, Value treeIndex) {
//   return mlir::Value();
// }

}
}