#ifndef _GPUSUPPORTUTILS_H_
#define _GPUSUPPORTUTILS_H_

#include "mlir/Dialect/GPU/ParallelLoopMapper.h"
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"

namespace mlir
{
namespace decisionforest
{

void GreedilyMapParallelLoopsToGPU(mlir::ModuleOp module);
void ConvertParallelLoopsToGPU(mlir::MLIRContext& context, mlir::ModuleOp module);

} // decisiontree
} // mlir


#endif // _GPUSUPPORTUTILS_H_