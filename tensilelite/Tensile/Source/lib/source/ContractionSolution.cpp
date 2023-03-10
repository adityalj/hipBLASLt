/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (C) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <Tensile/ContractionSolution.hpp>

#include <Tensile/hip/HipUtils.hpp>

#include <Tensile/AMDGPU.hpp>
#include <Tensile/ContractionProblem.hpp>
#include <Tensile/Utils.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace Tensile
{
    PerfModel perf;

    int32_t ContractionSolution::staggerUIter(ContractionSolution::Problem const& problem) const
    {
        uint32_t sizeL = problem.boundSize(0);

        // how many stride-sized clicks to stagger start offset
        unsigned int staggerUIter = sizeMapping.staggerU;

        // /DepthU/GSU
        int unrollLoopIters = sizeL / sizeMapping.depthU / sizeMapping.globalSplitU;

        unsigned int shifted = 1 << sizeMapping.staggerStrideShift;

        while(staggerUIter > 1)
        {
            if(unrollLoopIters >= (staggerUIter * shifted))
                break;

            staggerUIter /= 2; // step down to smaller stagger
        }

        if(staggerUIter >= 1)
            staggerUIter -= 1;

        return staggerUIter;
    }

    // Return magic number.  If magicShift is 0, compute and return it.
    uint32_t ContractionSolution::magicNumberAlg1(uint32_t x, uint32_t* magicShift) const
    {
        uint64_t magicNum;
        *magicShift = 33;
        magicNum    = (1L << *magicShift) / x + 1;
        if((magicNum >> 32) != 0)
        {
            *magicShift = 31;
            magicNum    = (1L << *magicShift) / x + 1;
        }

        assert(magicNum >> 32 == 0); // ensure magic number fits

        return static_cast<uint32_t>(magicNum);
    }

    uint32_t ContractionSolution::magicNumberAlg2(uint32_t d, uint32_t* magicShift) const
    {
        struct mu
        {
            unsigned M; // Magic number,
            int      a; // "add" indicator,
            int      s;
        }; // and shift amount.

        struct mu magu;
        if(d == 0)
        {
            // Make dividend of 0 return 0
            magu.M = 0;
            magu.a = 0;
            magu.s = 0;
        }
        else
        {
            // Must have 1 <= d <= 2**32-1.
            int      p;
            unsigned nc, delta, q1, r1, q2, r2;
            magu.a = 0; // Initialize "add" indicator.
            nc     = -1 - (-d) % d; // Unsigned arithmetic here.
            p      = 31; // Init. p.
            q1     = 0x80000000 / nc; // Init. q1 = 2**p/nc.
            r1     = 0x80000000 - q1 * nc; // Init. r1 = rem(2**p, nc).
            q2     = 0x7FFFFFFF / d; // Init. q2 = (2**p - 1)/d.
            r2     = 0x7FFFFFFF - q2 * d; // Init. r2 = rem(2**p - 1, d).
            do
            {
                p = p + 1;
                if(r1 >= nc - r1)
                {
                    q1 = 2 * q1 + 1; // Update q1.
                    r1 = 2 * r1 - nc;
                } // Update r1.
                else
                {
                    q1 = 2 * q1;
                    r1 = 2 * r1;
                }
                if(r2 + 1 >= d - r2)
                {
                    if(q2 >= 0x7FFFFFFF)
                        magu.a = 1;
                    q2 = 2 * q2 + 1; // Update q2.
                    r2 = 2 * r2 + 1 - d;
                } // Update r2.
                else
                {
                    if(q2 >= 0x80000000)
                        magu.a = 1;
                    q2 = 2 * q2;
                    r2 = 2 * r2 + 1;
                }
                delta = d - 1 - r2;
            } while(p < 64 && (q1 < delta || (q1 == delta && r1 == 0)));

            magu.M = q2 + 1; // Magic number
            magu.s = p - 32; // and shift amount to return
        }

        *magicShift         = magu.s;
        const uint32_t abit = 0x80000000;
        if(magu.a)
            *magicShift |= abit;

        // std::cout << " d=" << d << " M=" << magu.M << " a=" << magu.a << " s=" <<
        // magu.s << "\n";

        return magu.M;
    }

    uint32_t
        ContractionSolution::magicNumber(int magicDivAlg, uint32_t x, uint32_t* magicShift) const
    {
        if(magicDivAlg == 1)
            return magicNumberAlg1(x, magicShift);
        else if(magicDivAlg == 2)
            return magicNumberAlg2(x, magicShift);
        else
            throw std::runtime_error("bad magicDivAlg");
    }

    uint32_t ContractionSolution::smallMagicNumber(uint32_t x) const
    {
        uint64_t  magicNum;
        const int smallMagicShift = 31;
        magicNum                  = (1L << smallMagicShift) / x + 1;
        assert(magicNum >> 32 == 0); // ensure magic number fits
        return static_cast<uint32_t>(magicNum);
    }

    std::vector<size_t> generatePackedIndicesA(ContractionSolution::Problem const& problem,
                                               size_t                              packBatchDims)
    {
        std::vector<size_t> packedIndices;

        // TODO -move packedIndices calc to problem decode.
        for(auto idx = 0; idx < problem.a().dimensions(); idx++)
        {
            bool isSum = problem.boundIndices().end()
                         != std::find_if(problem.boundIndices().begin(),
                                         problem.boundIndices().end(),
                                         [idx](const ContractionProblemGemm::BoundIndex& bi) {
                                             return bi.a == idx;
                                         });

            bool nonPackableBatch = false;
            // TODO - base this check on if the batch is SetConstStrideA=0 - if so,
            // don't pack
            if(!(packBatchDims & 0x1))
            {
                nonPackableBatch
                    = problem.batchIndices().end()
                      != std::find_if(problem.batchIndices().begin(),
                                      problem.batchIndices().end(),
                                      [idx](const ContractionProblemGemm::BatchIndex& bi) {
                                          return bi.a == idx;
                                      });
            }

            if(!isSum && !nonPackableBatch)
                packedIndices.push_back(idx);
        }

        return packedIndices;
    }

    std::vector<size_t> generatePackedIndicesB(ContractionSolution::Problem const& problem,
                                               size_t                              packBatchDims)
    {
        std::vector<size_t> packedIndices;

        // Pack in all non-summation indices, except don't need magic number for the
        // last one
        for(auto idx = 0; idx < problem.b().dimensions(); idx++)
        {
            bool isSum = problem.boundIndices().end()
                         != std::find_if(problem.boundIndices().begin(),
                                         problem.boundIndices().end(),
                                         [idx](const ContractionProblemGemm::BoundIndex& bi) {
                                             return bi.b == idx;
                                         });

            bool nonPackableBatch = false;
            // TODO - base this check on if the batch is SetConstStrideB=0 - if so,
            // don't pack
            if(!(packBatchDims & 0x2))
            {
                nonPackableBatch
                    = problem.batchIndices().end()
                      != std::find_if(problem.batchIndices().begin(),
                                      problem.batchIndices().end(),
                                      [idx](const ContractionProblemGemm::BatchIndex& bi) {
                                          return bi.b == idx;
                                      });
            }

            if(!isSum && !nonPackableBatch)
                packedIndices.push_back(idx);
        }

        return packedIndices;
    }

    template <bool T_Debug>
    void ContractionSolution::singleCallArgs(ContractionSolution::Problem const& problem,
                                             ContractionInputs const&            inputs,
                                             uint32_t const&  problemNumGroupTiles0,
                                             uint32_t const&  problemNumGroupTiles1,
                                             bool const&      isGrouped,
                                             KernelArguments& args) const
    {
        if(debugKernel)
        {
            args.appendUnbound<unsigned int*>("debugBuffer");
        }

        TensorDescriptor const& a = problem.a();
        TensorDescriptor const& b = problem.b();
        TensorDescriptor const& c = problem.c();
        TensorDescriptor const& d = problem.d();

        uint64_t tensor2dSizeC = c.totalAllocatedElements();
        uint64_t tensor2dSizeA = (sizeMapping.packBatchDims & 0x1)
                                     ? a.totalAllocatedElements()
                                     : problem.allocatedElementsNonBatchA();
        uint64_t tensor2dSizeB = (sizeMapping.packBatchDims & 0x2)
                                     ? b.totalAllocatedElements()
                                     : problem.allocatedElementsNonBatchB();

        args.append<uint64_t>("tensor2dSizeC", tensor2dSizeC);
        args.append<uint64_t>("tensor2dSizeA", tensor2dSizeA);
        args.append<uint64_t>("tensor2dSizeB", tensor2dSizeB);

        if(sizeMapping.globalAccumulation)
        {
            args.append<void const*>("ws_d", inputs.ws);
            args.append<void const*>("ws_c", inputs.ws);
        }
        else if(problemType.stridedBatched)
        {
            args.append<void const*>("d", inputs.d);
            args.append<void const*>("c", inputs.c);
        }
        else
        {
            args.append<void const* const*>("batchD", inputs.batchD);
            args.append<void const* const*>("batchC", inputs.batchC);
        }

        if(problemType.stridedBatched)
        {
            args.append<void const*>("a", inputs.a);
            args.append<void const*>("b", inputs.b);
        }
        else
        {
            args.append<void const* const*>("batchA", inputs.batchA);
            args.append<void const* const*>("batchB", inputs.batchB);
        }

        args.append("alpha", inputs.alpha, problem.alphaType());
        if(problem.alphaType() == DataType::Half)
            args.append("alpha_2", inputs.alpha, problem.alphaType());

        if(problemType.useBeta)
        {
            args.append("beta", inputs.beta, problem.betaType());
            if(problem.betaType() == DataType::Half)
                args.append("beta_2", inputs.beta, problem.betaType());
        }

        if(problemType.useScaleD && (sizeMapping.globalSplitU == 1)) //kernel input data
        {
            args.append<void const*>("scaleD", inputs.scaleD);
        }

        size_t startStrideCD = problemType.useInitialStridesCD ? 0 : 1;
        size_t startStrideAB = problemType.useInitialStridesAB ? 0 : 1;

        if(sizeMapping.globalAccumulation)
        {
            size_t wsStride = startStrideCD ? d.sizes()[0] : 1;
            for(size_t i = startStrideCD; i < d.dimensions(); i++)
            {
                args.append<uint32_t>(concatenate_if<T_Debug>("strideW_D", i), wsStride);
                wsStride *= d.sizes()[i];
            }

            wsStride = startStrideCD ? d.sizes()[0] : 1;
            for(size_t i = startStrideCD; i < c.dimensions(); i++)
            {
                args.append<uint32_t>(concatenate_if<T_Debug>("strideW_C", i), wsStride);
                wsStride *= d.sizes()[i];
            }
        }
        else
        {
            for(size_t i = startStrideCD; i < d.dimensions(); i++)
                args.append<uint32_t>(concatenate_if<T_Debug>("strideD", i), d.strides()[i]);

            for(size_t i = startStrideCD; i < c.dimensions(); i++)
                args.append<uint32_t>(concatenate_if<T_Debug>("strideC", i), c.strides()[i]);
        }

        for(size_t i = startStrideAB; i < a.dimensions(); i++)
            args.append<uint32_t>(concatenate_if<T_Debug>("strideA", i), a.strides()[i]);

        for(size_t i = startStrideAB; i < b.dimensions(); i++)
            args.append<uint32_t>(concatenate_if<T_Debug>("strideB", i), b.strides()[i]);

        {
            int idx = 0;
            for(auto size : problem.problemSizes())
            {
                args.append<uint32_t>(concatenate_if<T_Debug>("size_", idx), size);
                idx++;
            }
        }

        args.append<int32_t>("staggerUIter", staggerUIter(problem));

        args.append<uint32_t>("problemNumGroupTiles0", problemNumGroupTiles0);
        args.append<uint32_t>("problemNumGroupTiles1", problemNumGroupTiles1);

        uint32_t numFullBlocks            = problemNumGroupTiles1;
        uint32_t wgmRemainder1            = 0;
        uint32_t magicNumberWgmRemainder1 = 0;

        if(sizeMapping.workGroupMapping != 0)
        {
            numFullBlocks = problemNumGroupTiles1 / sizeMapping.workGroupMapping;
            wgmRemainder1 = problemNumGroupTiles1 % sizeMapping.workGroupMapping;
            if(wgmRemainder1 == 0)
                wgmRemainder1 = sizeMapping.workGroupMapping;
            magicNumberWgmRemainder1 = smallMagicNumber(wgmRemainder1);
        }

        args.append<uint32_t>("numFullBlocks", numFullBlocks);
        args.append<uint32_t>("wgmRemainder1", wgmRemainder1);
        args.append<uint32_t>("magicNumberWgmRemainder1", magicNumberWgmRemainder1);

        args.append<uint32_t>("offsetD", d.offset());
        args.append<uint32_t>("offsetC", c.offset());
        args.append<uint32_t>("offsetA", a.offset());
        args.append<uint32_t>("offsetB", b.offset());

        if(isGrouped)
        {
            args.append<uint32_t>("SmallMagicNumberDivWg0",
                                  smallMagicNumber(problemNumGroupTiles0));
            args.append<uint32_t>("SmallMagicNumberDivWg01",
                                  smallMagicNumber(problemNumGroupTiles0 * problemNumGroupTiles1));
        }

        bool runActivation = false;
        if((problem.activationType() != ActivationType::None) && sizeMapping.activationFused
           && (sizeMapping.globalSplitU == 1))
            runActivation = true;
        if(problemType.useBias && (sizeMapping.globalSplitU == 1))
        {
            args.append<void const*>("bias", inputs.bias);
            if(runActivation)
            {
                size_t dummyInsertSize
                    = max(DataTypeInfo::Get(problem.d().dataType()).elementSize, 4) / 4 - 1;
                for(size_t i = 0; i < dummyInsertSize; i++)
                {
                    args.append<uint32_t>("bias_type_dummy", static_cast<uint32_t>(0));
                }
            }
            args.append<uint32_t>("bias_type", static_cast<uint32_t>(problem.biasType()));
        }

        if(runActivation)
        {
            for(int i = 0; i < inputs.activationArgs.size(); i++)
            {
                std::string name = "activation_" + std::to_string(i);
                if(problem.activationHPA()) // Same as hpa type.
                {
                    args.append(name.c_str(), inputs.activationArgs[i], problem.betaType());
                }
                else if(problem.d().dataType() == DataType::Half)
                {
                    args.append(
                        (name + "_pk").c_str(), inputs.activationArgs[i], problem.d().dataType());
                    args.append(name.c_str(), inputs.activationArgs[i], problem.d().dataType());
                }
                else
                {
                    if(problem.d().dataType() == DataType::BFloat16)
                    {
                        // BFloat16 to float32.
                        args.append<uint16_t>((name + "_append").c_str(), static_cast<uint16_t>(0));
                    }
                    args.append(name.c_str(), inputs.activationArgs[i], problem.d().dataType());
                }
            }
            if(problem.activationType() == ActivationType::All)
            {
                args.append<uint32_t>("activationType",
                                      static_cast<uint32_t>(problem.activationEnumArg()));
            }
        }
    }

    template <bool T_Debug>
    KernelInvocation
        ContractionSolution::generateSingleCall(ContractionSolution::Problem const& problem,
                                                ContractionInputs const&            inputs,
                                                Hardware const&                     hardware) const
    {
        TENSILE_ASSERT_EXC(sizeMapping.workGroupMapping >= 0);

        KernelInvocation rv;

        rv.args = KernelArguments(T_Debug);

        rv.args.reserve(1024, 128);

        rv.kernelName = kernelName;

        rv.workGroupSize.x = sizeMapping.workGroupSize.x * sizeMapping.workGroupSize.y
                             * sizeMapping.workGroupSize.z;
        rv.workGroupSize.y = 1;
        rv.workGroupSize.z = 1;

        rv.numWorkGroups.x = 1;
        rv.numWorkGroups.y = 1;

        for(size_t i = 0; i < problem.freeIndicesA().size(); i++)
        {
            rv.numWorkGroups.x *= problem.freeSizeA(i);
        }
        for(size_t i = 0; i < problem.freeIndicesB().size(); i++)
        {
            rv.numWorkGroups.y *= problem.freeSizeB(i);
        }

        rv.numWorkGroups.z = 1;
        for(size_t i = 0; i < problem.batchIndices().size(); i++)
        {
            if(sizeMapping.packBatchDims & 0x1)
                rv.numWorkGroups.x *= problem.batchSize(i);
            if(sizeMapping.packBatchDims & 0x2)
                rv.numWorkGroups.y *= problem.batchSize(i);
            if(!sizeMapping.packBatchDims)
                rv.numWorkGroups.z *= problem.batchSize(i);
        }

        if(problem.transposeC01())
            std::swap(rv.numWorkGroups.x, rv.numWorkGroups.y);

        rv.numWorkGroups.x = CeilDivide(rv.numWorkGroups.x, sizeMapping.macroTile.x);
        rv.numWorkGroups.y = CeilDivide(rv.numWorkGroups.y, sizeMapping.macroTile.y);

        uint32_t problemNumGroupTiles0 = rv.numWorkGroups.x;
        uint32_t problemNumGroupTiles1 = rv.numWorkGroups.y;
        // used only when persistent kernel along batch
        uint32_t problemNumGroupTiles2 = rv.numWorkGroups.z;

        rv.numWorkGroups.y *= sizeMapping.globalSplitU;

        rv.numWorkItems.x = rv.workGroupSize.x * rv.numWorkGroups.x;
        rv.numWorkItems.y = rv.workGroupSize.y * rv.numWorkGroups.y;
        rv.numWorkItems.z = rv.workGroupSize.z * rv.numWorkGroups.z;

        rv.sharedMemBytes = 0;

        singleCallArgs<T_Debug>(
            problem, inputs, problemNumGroupTiles0, problemNumGroupTiles1, false, rv.args);

        rv.codeObjectFile = codeObjectFilename.load();

        return rv;
    }

    template <bool T_Debug>
    KernelInvocation ContractionSolution::generateSingleCallGroupedGemm(
        std::vector<ContractionSolution::Problem> const& problems,
        ContractionSolution::GroupedInputs const&        inputs,
        Hardware const&                                  hardware) const
    {
        TENSILE_ASSERT_EXC(sizeMapping.workGroupMapping >= 0);
        KernelInvocation rv;
        rv.kernelName = kernelName;
        rv.args       = KernelArguments(T_Debug);
        auto args     = KernelArguments(T_Debug);
        args.reserve(1024, 128);
        std::vector<uint32_t> wg_table;
        uint32_t              wgLeft  = 0;
        uint32_t              wgRight = 0;

        rv.workGroupSize.x = sizeMapping.workGroupSize.x * sizeMapping.workGroupSize.y
                             * sizeMapping.workGroupSize.z;
        rv.workGroupSize.y = 1;
        rv.workGroupSize.z = 1;

        rv.numWorkItems.x = 0;
        rv.numWorkItems.y = 1;
        rv.numWorkItems.z = 1;

        for(int idx = 0; idx < problems.size(); idx++)
        {
            auto problem = problems[idx];

            rv.numWorkGroups.x = 1;
            rv.numWorkGroups.y = 1;
            rv.numWorkGroups.z = 1;

            for(size_t i = 0; i < problem.freeIndicesA().size(); i++)
            {
                rv.numWorkGroups.x *= problem.freeSizeA(i);
            }

            for(size_t i = 0; i < problem.freeIndicesB().size(); i++)
            {
                rv.numWorkGroups.y *= problem.freeSizeB(i);
            }

            for(size_t i = 0; i < problem.batchIndices().size(); i++)
            {
                if(sizeMapping.packBatchDims & 0x1)
                    rv.numWorkGroups.x *= problem.batchSize(i);
                if(sizeMapping.packBatchDims & 0x2)
                    rv.numWorkGroups.y *= problem.batchSize(i);
                if(!sizeMapping.packBatchDims)
                    rv.numWorkGroups.z *= problem.batchSize(i);
            }

            if(problem.transposeC01())
                std::swap(rv.numWorkGroups.x, rv.numWorkGroups.y);

            rv.numWorkGroups.x = CeilDivide(rv.numWorkGroups.x, sizeMapping.macroTile.x);
            rv.numWorkGroups.y = CeilDivide(rv.numWorkGroups.y, sizeMapping.macroTile.y);

            uint32_t problemNumGroupTiles0 = rv.numWorkGroups.x;
            uint32_t problemNumGroupTiles1 = rv.numWorkGroups.y;

            rv.numWorkItems.x += (rv.workGroupSize.x * rv.numWorkGroups.x * rv.workGroupSize.y
                                  * rv.numWorkGroups.y * sizeMapping.globalSplitU
                                  * rv.workGroupSize.y * rv.numWorkGroups.z);

            wgRight
                = rv.numWorkItems.x / rv.workGroupSize.x / rv.workGroupSize.y / rv.workGroupSize.y;
            wg_table.push_back(wgLeft);
            wgLeft = wgRight;

            rv.sharedMemBytes = 0;

            singleCallArgs<T_Debug>(problem,
                                    inputs.grouped[idx],
                                    problemNumGroupTiles0,
                                    problemNumGroupTiles1,
                                    true,
                                    args);
        }

        std::vector<uint8_t> h_args;
        h_args.resize((args.size() * sizeof(uint8_t) + wg_table.size() * sizeof(uint32_t))
                      / sizeof(uint8_t));
        std::memcpy(&h_args[0], wg_table.data(), wg_table.size() * sizeof(uint32_t));
        std::memcpy(&h_args[wg_table.size() * sizeof(uint32_t) / sizeof(uint8_t)],
                    args.data(),
                    args.size() * sizeof(uint8_t));

        rv.args.append<uint32_t>("numGemms", problems.size());
        uint8_t* d_args = (uint8_t*)(inputs.grouped[0].ws);
        HIP_CHECK_EXC(hipMemcpy(
            d_args, h_args.data(), h_args.size() * sizeof(uint8_t), hipMemcpyHostToDevice));
        rv.args.append<uint8_t const*>("args", d_args);
        rv.codeObjectFile = codeObjectFilename.load();

        return rv;
    }

    template <bool T_Debug>
    KernelInvocation ContractionSolution::generateBetaOnlyCall(Problem const&           problem,
                                                               ContractionInputs const& inputs,
                                                               Hardware const& hardware) const
    {
        TensorDescriptor const& c = problem.c();
        TensorDescriptor const& d = problem.d();

        KernelInvocation rv;

        rv.args = KernelArguments(T_Debug);

        rv.args.reserve(512, 64);

        rv.kernelName = betaOnlyKernelName(problem, inputs, hardware);

        rv.workGroupSize.x = 256;
        rv.workGroupSize.y = 1;
        rv.workGroupSize.z = 1;

        size_t wiX = 1;
        size_t wiY = 1;
        size_t wiZ = 1;
        for(size_t i = 0; i < problem.freeIndicesA().size(); i++)
            wiX *= problem.freeSizeA(i);
        for(size_t i = 0; i < problem.freeIndicesB().size(); i++)
            wiY *= problem.freeSizeB(i);
        for(size_t i = 0; i < problem.batchIndices().size(); i++)
            wiZ *= problem.batchSize(i);

        rv.numWorkGroups.x = CeilDivide(wiX * wiY * wiZ, rv.workGroupSize.x);
        rv.numWorkGroups.y = 1;
        rv.numWorkGroups.z = 1;

        rv.numWorkItems.x = rv.workGroupSize.x * rv.numWorkGroups.x;
        rv.numWorkItems.y = rv.workGroupSize.y * rv.numWorkGroups.y;
        rv.numWorkItems.z = rv.workGroupSize.z * rv.numWorkGroups.z;

        if(sizeMapping.globalAccumulation)
            rv.args.append<void*>("WS", inputs.ws);
        else if(problemType.stridedBatched)
            rv.args.append<void*>("D", inputs.d);
        else
            rv.args.append<void const* const*>("batchD", inputs.batchD);

        if(problemType.stridedBatched)
            rv.args.append<void const*>("C", inputs.c);
        else
            rv.args.append<void const* const*>("batchC", inputs.batchC);

        if(problemType.useBias && sizeMapping.globalAccumulation == 0)
        {
            rv.args.append<void const*>("bias", inputs.bias);
        }
        if(problemType.useScaleD && sizeMapping.globalAccumulation == 0)
        {
            rv.args.append<void const*>("scaleD", inputs.scaleD);
        }

        if(sizeMapping.globalAccumulation)
        {
            size_t stride = d.sizes()[0];
            for(size_t i = 1; i < d.dimensions(); i++)
            {
                rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideW", i),
                                         d.sizes()[i] == 1 ? 0 : stride);
                stride *= d.sizes()[i];
            }
        }
        else
        {
            for(size_t i = 1; i < d.dimensions(); i++)
                rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideD", i),
                                         d.sizes()[i] == 1 ? 0 : d.strides()[i]);
        }

        for(size_t i = 1; i < c.dimensions(); i++)
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideC", i),
                                     c.sizes()[i] == 1 ? 0 : c.strides()[i]);

        int idx = 0;
        for(auto size : problem.d().sizes())
        {
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("size_", idx), size);
            idx++;
        }

        rv.args.append<uint32_t>("offsetD", d.offset());
        rv.args.append<uint32_t>("offsetC", c.offset());

        rv.args.append("beta", inputs.beta, problem.betaType());

        //Pass along code object dependency
        rv.codeObjectFile = codeObjectFilename.load();

        return rv;
    }

    std::string ContractionSolution::betaOnlyKernelName(Problem const&           problem,
                                                        ContractionInputs const& inputs,
                                                        Hardware const&          hardware) const
    {
        std::string name = concatenate(
            "C", problem.cNames(), "_", DataTypeInfo::Get(problem.d().dataType()).abbrev);

        if(problemType.groupedGemm)
        {
            name += "_GG";
        }
        else if(!problemType.stridedBatched)
        {
            name += "_GB";
        }

        if(problemType.useBias && (sizeMapping.globalAccumulation == 0))
        {
            auto s = TypeAbbrev(problem.biasType());
            name += ("_Bias" + s);
        }

        if(sizeMapping.globalAccumulation)
        {
            name += "_GA";
        }

        return name;
    }

    template <bool T_Debug>
    KernelInvocation ContractionSolution::generateOutputConversionCall(
        Problem const& problem, ContractionInputs const& inputs, Hardware const& hardware) const
    {
        TensorDescriptor const& c = problem.c();
        TensorDescriptor const& d = problem.d();

        KernelInvocation rv;

        rv.args = KernelArguments(T_Debug);

        rv.args.reserve(512, 64);

        rv.kernelName = outputConversionKernelName(problem, inputs, hardware);

        rv.workGroupSize.x = 256;
        rv.workGroupSize.y = 1;
        rv.workGroupSize.z = 1;

        size_t wiX = 1;
        size_t wiY = 1;
        size_t wiZ = 1;
        for(size_t i = 0; i < problem.freeIndicesA().size(); i++)
            wiX *= problem.freeSizeA(i);
        for(size_t i = 0; i < problem.freeIndicesB().size(); i++)
            wiY *= problem.freeSizeB(i);
        for(size_t i = 0; i < problem.batchIndices().size(); i++)
            wiZ *= problem.batchSize(i);

        rv.numWorkGroups.x = CeilDivide(wiX * wiY * wiZ, rv.workGroupSize.x);
        rv.numWorkGroups.y = 1;
        rv.numWorkGroups.z = 1;

        rv.numWorkItems.x = rv.workGroupSize.x * rv.numWorkGroups.x;
        rv.numWorkItems.y = rv.workGroupSize.y * rv.numWorkGroups.y;
        rv.numWorkItems.z = rv.workGroupSize.z * rv.numWorkGroups.z;

        if(problemType.stridedBatched)
            rv.args.append<void*>("D", inputs.d);
        else
            rv.args.append<void const* const*>("batchD", inputs.batchD);

        rv.args.append<void*>("WS", inputs.ws);

        if(problemType.stridedBatched)
            rv.args.append<void const*>("C", inputs.c);
        else
            rv.args.append<void const* const*>("batchC", inputs.batchC);

        if(problemType.useBias)
        {
            rv.args.append<void const*>("bias", inputs.bias);
        }
        if(problemType.useScaleD) // GSU dep
        {
            rv.args.append<void const*>("scaleD", inputs.scaleD);
        }

        if(sizeMapping.globalAccumulation == 2)
            rv.args.append("alpha", inputs.alpha, problem.alphaType());
        else
            rv.args.append("alpha", 1.0f, problem.betaType());

        if(sizeMapping.globalAccumulation == 2 and problemType.useBeta)
            rv.args.append("beta", inputs.beta, problem.betaType());
        else
            rv.args.append("beta", 0.0f, problem.betaType());

        if((problem.activationType() != ActivationType::None) && sizeMapping.activationFused)
        {
            for(int i = 0; i < inputs.activationArgs.size(); i++)
            {
                std::string name = "activation_" + std::to_string(i);
                if(problem.activationHPA()) // Same as hpa type.
                {
                    rv.args.append(name.c_str(), inputs.activationArgs[i], problem.betaType());
                }
                else if(problem.d().dataType() == DataType::BFloat16)
                {
                    rv.args.append<float>(
                        name.c_str(),
                        static_cast<float>((*std::get_if<BFloat16>(&inputs.activationArgs[i]))));
                }
                else
                {
                    rv.args.append(name.c_str(), inputs.activationArgs[i], problem.d().dataType());
                }
            }
            if(problem.activationType() == ActivationType::All)
            {
                rv.args.append<uint32_t>("activationType",
                                         static_cast<uint32_t>(problem.activationEnumArg()));
            }
        }

        for(size_t i = 1; i < d.dimensions(); i++)
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideD", i), d.strides()[i]);

        uint32_t wsStride = d.sizes()[0];
        for(size_t i = 1; i < d.dimensions(); i++)
        {
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideW", i), wsStride);
            wsStride *= d.sizes()[i];
        }

        for(size_t i = 1; i < c.dimensions(); i++)
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideC", i), c.strides()[i]);

        int idx = 0;
        for(auto size : problem.d().sizes())
        {
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("size_", idx), size);
            idx++;
        }

        rv.args.append<uint32_t>("offsetD", d.offset());
        rv.args.append<uint32_t>("offsetC", c.offset());

        if(sizeMapping.globalAccumulation == 1)
            rv.args.append<uint32_t>("gsu", 1);
        else
            rv.args.append<uint32_t>("gsu", sizeMapping.globalSplitU);

        //@TODO determine if this is needed, may not end up in the same code object file
        rv.codeObjectFile = codeObjectFilename.load();

        return rv;
    }

    std::string ContractionSolution::outputConversionKernelName(Problem const&           problem,
                                                                ContractionInputs const& inputs,
                                                                Hardware const& hardware) const
    {
        std::string name = concatenate(
            "C", problem.cNames(), "_", DataTypeInfo::Get(problem.d().dataType()).abbrev);

        if(problemType.groupedGemm)
        {
            name += "_GG";
        }
        else if(!problemType.stridedBatched)
        {
            name += "_GB";
        }

        if(problemType.useBias)
        {
            auto s = TypeAbbrev(problem.biasType());
            name += ("_Bias" + s);
        }

        if(problem.activationType() != ActivationType::None)
        {
            if(problem.activationType() == ActivationType::All)
                name += "_A";
            else
            {
                std::string actName = ToString(problem.activationType());
                std::transform(actName.begin(), actName.end(), actName.begin(), ::toupper);
                name += actName;
            }
        }
        if(problem.activationHPA())
        {
            name += "h";
        }

        if(problemType.useScaleD)
        {
            name += ("_ScaleD");
        }

        name += "_PostGSU";

        return name;
    }

    template <bool T_Debug>
    KernelInvocation ContractionSolution::generateActivationOnlyCall(
        Problem const& problem, ContractionInputs const& inputs, Hardware const& hardware) const
    {
        TensorDescriptor const& d = problem.d();

        KernelInvocation rv;

        rv.args = KernelArguments(T_Debug);

        rv.args.reserve(512, 64);

        rv.kernelName = activationOnlyKernelName(problem, inputs, hardware);

        rv.workGroupSize.x = 256;
        rv.workGroupSize.y = 1;
        rv.workGroupSize.z = 1;

        size_t wiX = 1;
        size_t wiY = 1;
        size_t wiZ = 1;
        for(size_t i = 0; i < problem.freeIndicesA().size(); i++)
            wiX *= problem.freeSizeA(i);
        for(size_t i = 0; i < problem.freeIndicesB().size(); i++)
            wiY *= problem.freeSizeB(i);
        for(size_t i = 0; i < problem.batchIndices().size(); i++)
            wiZ *= problem.batchSize(i);

        rv.numWorkGroups.x = CeilDivide(wiX * wiY * wiZ, rv.workGroupSize.x);
        rv.numWorkGroups.y = 1;
        rv.numWorkGroups.z = 1;

        rv.numWorkItems.x = rv.workGroupSize.x * rv.numWorkGroups.x;
        rv.numWorkItems.y = rv.workGroupSize.y * rv.numWorkGroups.y;
        rv.numWorkItems.z = rv.workGroupSize.z * rv.numWorkGroups.z;

        if(problemType.stridedBatched)
            rv.args.append<void*>("D", inputs.d);
        else
            rv.args.append<void const* const*>("batchD", inputs.batchD);

        if(problem.activationType() != ActivationType::None)
        {
            if(problem.activationType() == ActivationType::All)
            {
                rv.args.append<uint32_t>("activationType",
                                         static_cast<uint32_t>(problem.activationEnumArg()));
            }
            for(int i = 0; i < inputs.activationArgs.size(); i++)
            {
                std::string name = "activation_" + std::to_string(i);
                if(problem.activationHPA()) // Same as hpa type.
                {
                    rv.args.append(name.c_str(), inputs.activationArgs[i], problem.betaType());
                }
                else if(problem.d().dataType() == DataType::BFloat16)
                {
                    rv.args.append<float>(
                        name.c_str(),
                        static_cast<float>(*std::get_if<BFloat16>(&inputs.activationArgs[i])));
                }
                else
                {
                    rv.args.append(name.c_str(), inputs.activationArgs[i], problem.d().dataType());
                }
            }
        }

        if(sizeMapping.globalAccumulation)
        {
            size_t stride = d.sizes()[0];
            for(size_t i = 1; i < d.dimensions(); i++)
            {
                rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideW", i),
                                         d.sizes()[i] == 1 ? 0 : stride);
                stride *= d.sizes()[i];
            }
        }
        else
        {
            for(size_t i = 1; i < d.dimensions(); i++)
                rv.args.append<uint32_t>(concatenate_if<T_Debug>("strideD", i),
                                         d.sizes()[i] == 1 ? 0 : d.strides()[i]);
        }

        int idx = 0;
        for(auto size : problem.d().sizes())
        {
            rv.args.append<uint32_t>(concatenate_if<T_Debug>("size_", idx), size);
            idx++;
        }

        rv.args.append<uint32_t>("offsetD", d.offset());

        return rv;
    }

    std::string ContractionSolution::activationOnlyKernelName(Problem const&           problem,
                                                              ContractionInputs const& inputs,
                                                              Hardware const& hardware) const
    {
        std::string name = concatenate(
            "D", problem.cNames(), "_", DataTypeInfo::Get(problem.d().dataType()).abbrev);
        if(problem.activationType() != ActivationType::None)
        {
            if(problem.activationType() == ActivationType::All)
                name += "_A";
            else
            {
                std::string actName = ToString(problem.activationType());
                std::transform(actName.begin(), actName.end(), actName.begin(), ::toupper);
                name += actName;
            }
        }
        if(problem.activationHPA())
        {
            name += "h";
        }

        return name;
    }

    std::vector<KernelInvocation> ContractionSolution::solve(ContractionProblem const& problem,
                                                             ProblemInputs const&      inputs,
                                                             Hardware const& hardware) const
    {
        if(auto gemmProblem = dynamic_cast<ContractionProblemGemm const*>(&problem))
        {
            auto gemmInputs = dynamic_cast<ContractionInputs const*>(&inputs);
            return solve((*gemmProblem), (*gemmInputs), hardware);
        }
        else if(auto groupedProblem = dynamic_cast<ContractionProblemGroupedGemm const*>(&problem))
        {
            auto& gemms         = groupedProblem->gemms;
            auto  groupedInputs = dynamic_cast<ContractionGroupedInputs const*>(&inputs);
            return solveGroupedGemm(gemms, (*groupedInputs), hardware);
        }
        else
        {
            throw std::runtime_error("Failed to cast problem type.");
        }
    }

    std::vector<KernelInvocation>
        ContractionSolution::solve(ContractionSolution::Problem const& problem,
                                   ContractionSolution::Inputs const&  inputs,
                                   Hardware const&                     hardware) const
    {
        if(Debug::Instance().printWinningKernelName())
            std::cout << "Running kernel: " << this->KernelName() << std::endl;

        // retreive alpha/beta type set via setAlpha/BetaType()
        auto alphaType = problem.alphaType();
        auto betaType  = problem.betaType();
        auto biasType  = problem.biasType();

        // TODO: Some gtests are passing the "problem" without actually defining the
        // alpha/beta type (alphaType and betaType remain None).
        // Until we fix those gtests, we need to keep this condition to adjust the missing
        // alpha/beta data types.
        if(alphaType == DataType::None)
        {
            alphaType
                = problemType.aType == DataType::BFloat16 ? DataType::Float : problemType.dType;
        }
        if(betaType == DataType::None)
        {
            betaType = alphaType;
        }
        if(biasType == DataType::None)
        {
            biasType = problemType.dType;
        }

        bool debug = Debug::Instance().printKernelArguments() || this->kernelArgsLog;

        int boundSize = 1;
        for(size_t i = 0; i < problem.boundIndices().size(); i++)
            boundSize *= problem.boundSize(i);

        // Check for nullptrs if alpha is non-zero.
        if((!CompareValue(inputs.alpha, (double)0) && (boundSize != 0))
           && ((problem.stridedBatched() && (inputs.a == nullptr || inputs.b == nullptr))
               || (!problem.stridedBatched()
                   && (inputs.batchA == nullptr || inputs.batchB == nullptr))))
        {
            std::string matrixID = inputs.a == nullptr ? "A" : "B";
            std::string msg      = std::string("Unsupported nullptr for ") + matrixID
                              + std::string(" when (Alpha !=0) && (K != 0)\n");
            throw std::runtime_error(msg.c_str());
        }

        // Check if alpha matches problem definition
        if(problem.alphaRestriction() != ScalarValue::Any
           && problem.alphaRestriction() != toScalarValueEnum(inputs.alpha))
        {
            std::stringstream inputValue;
            inputValue << ToString(inputs.alpha);
            std::string msg = std::string("Alpha value ") + inputValue.str()
                              + std::string(" doesn't match that set in problem: ")
                              + ToString(problem.alphaRestriction());
            throw std::runtime_error(msg.c_str());
        }

        // Check if beta matches problem definition
        if(problem.betaRestriction() != ScalarValue::Any
           && problem.betaRestriction() != toScalarValueEnum(inputs.beta))
        {
            std::stringstream inputValue;
            inputValue << ToString(inputs.beta);
            std::string msg = std::string("Beta value ") + inputValue.str()
                              + std::string(" doesn't match that set in problem: ")
                              + ToString(problem.betaRestriction());
            throw std::runtime_error(msg.c_str());
        }

        if(problem.cEqualsD() && inputs.c != inputs.d)
            throw std::runtime_error(
                "ContractionProblemGemm has cEqualsD set, but pointers for c and d are not equal");

        std::vector<KernelInvocation> rv;

        if(sizeMapping.globalSplitU > 1 && sizeMapping.globalAccumulation != 2)
        {
            if(debug)
                rv.push_back(generateBetaOnlyCall<true>(problem, inputs, hardware));
            else
                rv.push_back(generateBetaOnlyCall<false>(problem, inputs, hardware));
        }

        if(debug)
            rv.push_back(generateSingleCall<true>(problem, inputs, hardware));
        else
            rv.push_back(generateSingleCall<false>(problem, inputs, hardware));

        if(sizeMapping.globalAccumulation)
        {
            if(debug)
                rv.push_back(generateOutputConversionCall<true>(problem, inputs, hardware));
            else
                rv.push_back(generateOutputConversionCall<false>(problem, inputs, hardware));
        }

        if(((sizeMapping.globalSplitU > 1 && (!sizeMapping.globalAccumulation))
            || (!sizeMapping.activationFused))
           && (problem.activationType() != ActivationType::None))
        {
            if(debug)
                rv.push_back(generateActivationOnlyCall<true>(problem, inputs, hardware));
            else
                rv.push_back(generateActivationOnlyCall<false>(problem, inputs, hardware));
        }

        return rv;
    }

    std::vector<KernelInvocation> ContractionSolution::solveGroupedGemm(
        std::vector<ContractionSolution::Problem> const& problems,
        ContractionSolution::GroupedInputs const&        inputs,
        Hardware const&                                  hardware) const
    {
        if(Debug::Instance().printWinningKernelName())
            std::cout << "Running kernel: " << this->KernelName() << std::endl;

        // retreive alpha/beta type set via setAlpha/BetaType()
        auto alphaType = problems[0].alphaType();
        auto betaType  = problems[0].betaType();
        auto biasType  = problems[0].biasType();

        // TODO: Some gtests are passing the "problem" without actually defining the
        // alpha/beta type (alphaType and betaType remain None).
        // Until we fix those gtests, we need to keep this condition to adjust the missing
        // alpha/beta data types.
        if(alphaType == DataType::None)
        {
            alphaType
                = problemType.aType == DataType::BFloat16 ? DataType::Float : problemType.dType;
        }
        if(betaType == DataType::None)
        {
            betaType = alphaType;
        }
        if(biasType == DataType::None)
        {
            biasType = problemType.dType;
        }

        bool debug = Debug::Instance().printKernelArguments() || this->kernelArgsLog;

        // Check for nullptrs if alpha is non-zero.
        for(int idx = 0; idx < problems.size(); idx++)
        {
            int boundSize = 1;
            for(size_t i = 0; i < problems[idx].boundIndices().size(); i++)
                boundSize *= problems[idx].boundSize(i);

            if(((!CompareValue(inputs.grouped[idx].alpha, (double)0)) && (boundSize != 0))
               && ((problems[idx].stridedBatched()
                    && (inputs.grouped[idx].a == nullptr || inputs.grouped[idx].b == nullptr))))
            {
                std::string matrixID = inputs.grouped[idx].a == nullptr ? "A" : "B";
                std::string msg      = std::string("Unsupported nullptr for ") + matrixID
                                  + std::string(" when (Alpha !=0) && (K != 0)\n");
                throw std::runtime_error(msg.c_str());
            }

            // Check if alpha matches problem definition
            if(problems[idx].alphaRestriction() != ScalarValue::Any
               && problems[idx].alphaRestriction() != toScalarValueEnum(inputs.grouped[idx].alpha))
            {
                std::stringstream inputValue;
                inputValue << ToString(inputs.grouped[idx].alpha);
                std::string msg = std::string("Alpha value ") + inputValue.str()
                                  + std::string(" doesn't match that set in problem: ")
                                  + ToString(problems[idx].alphaRestriction());
                throw std::runtime_error(msg.c_str());
            }

            // Check if beta matches problem definition
            if(problems[idx].betaRestriction() != ScalarValue::Any
               && problems[idx].betaRestriction() != toScalarValueEnum(inputs.grouped[idx].beta))
            {
                std::stringstream inputValue;
                inputValue << ToString(inputs.grouped[idx].beta);
                std::string msg = std::string("Beta value ") + inputValue.str()
                                  + std::string(" doesn't match that set in problem: ")
                                  + ToString(problems[idx].betaRestriction());
                throw std::runtime_error(msg.c_str());
            }

            if(problems[idx].cEqualsD() && inputs.grouped[idx].c != inputs.grouped[idx].d)
                throw std::runtime_error(
                    "ContractionProblem has cEqualsD set, but pointers for c and d are not equal");
        }

        std::vector<KernelInvocation> rv;

        if(debug)
            rv.push_back(generateSingleCallGroupedGemm<true>(problems, inputs, hardware));
        else
            rv.push_back(generateSingleCallGroupedGemm<false>(problems, inputs, hardware));

        return rv;
    }

    ContractionSolution::StaticPerformanceModel
        ContractionSolution::staticPerformanceModel(double M,
                                                    double N,
                                                    double K,
                                                    double NumBatches,
                                                    double MT0,
                                                    double MT1,
                                                    double NumCUs,
                                                    double TotalGranularity,
                                                    int    GlobalSplitU) const
    {
        StaticPerformanceModel spm;

        int beta      = (int)problemType.useBeta;
        int betaReads = 0, betaWrites = 0;
        if(GlobalSplitU == 1)
        {
            if(beta != 0.0)
                betaReads = 1.0;
        }
        else
        {
            if(beta == 0)
                betaWrites = 1; // zero output
            else if(beta != 1.0) // if 1.0, just atomic update output
            {
                // if not 1.0, read, scale, write, then atomic update in kernel
                betaReads  = 1; // initial read for scale
                betaWrites = 1; // writeback after scale
            }
        }

        auto aInfo = DataTypeInfo::Get(problemType.aType);
        auto bInfo = DataTypeInfo::Get(problemType.bType);
        auto cInfo = DataTypeInfo::Get(problemType.cType);
        auto dInfo = DataTypeInfo::Get(problemType.dType);

        spm.memReadBytesA = (NumBatches * M * N * K) / MT1 * aInfo.elementSize;
        spm.memReadBytesB = (NumBatches * M * N * K) / MT0 * bInfo.elementSize;
        spm.memReadBytesC = (NumBatches * M * N) * betaReads * cInfo.elementSize;

        if(GlobalSplitU == 1)
            spm.memWriteBytesD = (NumBatches * M * N) * (1 + betaWrites) * dInfo.elementSize;
        else
        {
            bool   hardwareAtomic   = false; // TODO-model
            double atomicOperations = hardwareAtomic ? 2 : 3; // read-mod-write or cas  //TODO-model
            double atomicCollisions = 1.0; // TODO-could be based on K, GSU
            spm.memWriteBytesD      = (NumBatches * M * N)
                                 * (betaWrites + atomicOperations * atomicCollisions)
                                 * dInfo.elementSize;
        }
        spm.memReadBytes   = spm.memReadBytesA + spm.memReadBytesB + spm.memReadBytesC;
        spm.memGlobalReads = spm.memReadBytesA / aInfo.elementSize
                             + spm.memReadBytesB / bInfo.elementSize
                             + spm.memReadBytesC / cInfo.elementSize;
        spm.memGlobalWrites = spm.memWriteBytesD / dInfo.elementSize;

        return spm;
    }

    size_t ContractionSolution::requiredWorkspaceSize(Problem const& problem) const
    {
        size_t size = 0;

        size += problem.d().totalLogicalElements() * sizeMapping.workspaceSizePerElemC;

        return size;
    }

    float ContractionSolution::computeGranularity(float x)
    {
        return x / ceil(x);
    }

    ContractionSolution::Granularities ContractionSolution::computeGranularities(
        Hardware const& hardware, double M, double N, double K, double NumBatches) const
    {
        ContractionSolution::Granularities granularities;

        double MT0 = sizeMapping.macroTile.x;
        double MT1 = sizeMapping.macroTile.y;

        AMDGPU const* pAMDGPU = dynamic_cast<AMDGPU const*>(&hardware);
        assert(pAMDGPU);

        double NumCUs        = pAMDGPU->computeUnitCount;
        double wavefrontSize = pAMDGPU->wavefrontSize;
        double simdPerCu     = pAMDGPU->simdPerCu;

        double GlobalSplitU = sizeMapping.globalSplitU;
        double LocalSplitU  = sizeMapping.workGroupSize.z;

        granularities.MT0 = MT0;
        granularities.MT1 = MT1;
        granularities.GSU = GlobalSplitU;
        granularities.LSU = LocalSplitU;
        granularities.CUs = NumCUs;

        granularities.numTiles0 = M / MT0;
        granularities.numTiles1 = N / MT1;

        granularities.tile0Granularity = computeGranularity(granularities.numTiles0);
        granularities.tile1Granularity = computeGranularity(granularities.numTiles1);

        granularities.tilesPerCu
            = (NumBatches * ceil(granularities.numTiles0) * ceil(granularities.numTiles1))
              / (NumCUs / GlobalSplitU / LocalSplitU);

        granularities.totalTiles    = ceil(granularities.numTiles0) * ceil(granularities.numTiles1);
        granularities.natTilesPerCu = NumBatches * granularities.totalTiles / NumCUs;
        granularities.suTilesPerCu  = (granularities.totalTiles * GlobalSplitU) / NumCUs;
        granularities.suCuGranularity = computeGranularity(granularities.suTilesPerCu);

        granularities.waveGranularity = std::min(
            1.00,
            static_cast<double>(floor(granularities.tilesPerCu + 1.0) * sizeMapping.workGroupSize.x
                                * sizeMapping.workGroupSize.y * sizeMapping.workGroupSize.z)
                / pAMDGPU->wavefrontSize / pAMDGPU->simdPerCu);

        granularities.waves
            = ceil((sizeMapping.workGroupSize.x * sizeMapping.workGroupSize.y) / wavefrontSize);

        granularities.suWavesPerSimdx2
            = (granularities.suTilesPerCu * granularities.waves) / (2 * simdPerCu);
        granularities.suWaveGranularity
            = granularities.suWavesPerSimdx2 * ceil(granularities.suWavesPerSimdx2);

        double nat_tiles_per_cu
            = NumBatches * ceil(granularities.numTiles0) * ceil(granularities.numTiles1) / NumCUs;
        granularities.natCuGranularity = ceil(nat_tiles_per_cu) * ceil(nat_tiles_per_cu) / NumCUs;

        granularities.cuGranularity = computeGranularity(granularities.tilesPerCu);

        granularities.totalGranularity
            = granularities.tile0Granularity * granularities.tile1Granularity
              * granularities.cuGranularity * granularities.waveGranularity;

        granularities.totalTileAwareGranularity
            = granularities.tile0Granularity * granularities.tile1Granularity
              * granularities.suCuGranularity * granularities.suWaveGranularity;

        return granularities;
    }

    ContractionSolution::ProjectedPerformance
        ContractionSolution::projectedPerformance(Problem const&  problem,
                                                  Hardware const& hardware) const
    {
        ProjectedPerformance pp;

        double M = 1.0, N = 1.0;
        if(problem.freeIndicesA().size() > 1 || sizeMapping.packBatchDims & 0x1)
        {
            std::vector<size_t> packedIndices
                = generatePackedIndicesA(problem, sizeMapping.packBatchDims);
            for(auto pi = packedIndices.begin(); pi != packedIndices.end(); pi++)
                M *= problem.a().sizes()[*pi];
        }
        else
            M = problem.freeSizeA(0);

        if(problem.freeIndicesB().size() > 1 || sizeMapping.packBatchDims & 0x2)
        {
            std::vector<size_t> packedIndices
                = generatePackedIndicesB(problem, sizeMapping.packBatchDims);
            for(auto pi = packedIndices.begin(); pi != packedIndices.end(); pi++)
                N *= problem.b().sizes()[*pi];
        }
        else
            N = problem.freeSizeB(0);

        double NumBatches = 1;
        if(sizeMapping.packBatchDims == 0)
        {
            for(size_t i = 0; i < problem.batchIndices().size(); i++)
                NumBatches *= problem.batchSize(i);
        }
        double K = problem.boundSize(0); // TODO - fix for multiple summations

        pp.granularities = ContractionSolution::computeGranularities(hardware, M, N, K, NumBatches);

        auto it = ideals.begin();

        int    closestKMeasure     = std::numeric_limits<int>::max();
        double closestKPerformance = 0.0;

        while(it != ideals.end())
        {
            int myK       = it->first;
            int myMeasure = std::abs(myK - K);
            if(myMeasure < closestKMeasure)
            {
                closestKMeasure     = myMeasure;
                closestKPerformance = it->second;
            }
            it++;
        }

        double MT0    = pp.granularities.MT0;
        double MT1    = pp.granularities.MT1;
        double NumCUs = pp.granularities.CUs;

        double GlobalSplitU         = pp.granularities.GSU;
        double IdealGranularityPerf = closestKPerformance;

        pp.staticModel = staticPerformanceModel(
            M, N, K, NumBatches, MT0, MT1, NumCUs, pp.granularities.totalGranularity, GlobalSplitU);

        pp.speedGFlops = IdealGranularityPerf * pp.granularities.totalGranularity;
        pp.CUs         = NumCUs;

        return pp;
    }

    ContractionSolution::TAMetricProblemScore ContractionSolution::computeProblemScore(
        Hardware const& hardware, double M, double N, double K, double NumBatches) const
    {
        ContractionSolution::TAMetricProblemScore pp;
        pp.granularites = ContractionSolution::computeGranularities(hardware, M, N, K, NumBatches);

        pp.M = M;
        pp.N = N;
        pp.K = K;

        double slope     = linearModel.slope;
        double intercept = linearModel.intercept;
        double perf_max  = linearModel.max;

        double sum_value        = K;
        double sum_perf0        = sum_value / (intercept + (slope * sum_value));
        pp.summationPerformance = 1000.0 * sum_perf0 / perf_max;

        return pp;
    }

    double ContractionSolution::computeTileAwareMetric(
        ContractionSolution::TAMetricProblemScore pp,
        ContractionSolution::TAMetricProblemScore ppReference) const
    {
        double tile0GranularityDim = abs(log(ppReference.granularites.tile0Granularity)
                                         - log(pp.granularites.tile0Granularity));
        double metric              = tile0GranularityDim;

        double tile1GranularityDim = abs(log(ppReference.granularites.tile1Granularity)
                                         - log(pp.granularites.tile1Granularity));
        metric += tile1GranularityDim;

        double natCuGranularityDim = abs(log(ppReference.granularites.natCuGranularity)
                                         - log(pp.granularites.natCuGranularity));
        metric += natCuGranularityDim;

        double suCuGranularityDim = abs(log(ppReference.granularites.suCuGranularity)
                                        - log(pp.granularites.suCuGranularity));
        metric += suCuGranularityDim;

        double suWaveGranularityDim = abs(log(ppReference.granularites.suWaveGranularity)
                                          - log(pp.granularites.suWaveGranularity));
        metric += suWaveGranularityDim;

        double natTilesPerCuDim
            = abs(log(ppReference.granularites.natTilesPerCu) - log(pp.granularites.natTilesPerCu));
        metric += natTilesPerCuDim;

        double suTilesPerCuDim
            = abs(log(ppReference.granularites.suTilesPerCu) - log(pp.granularites.suTilesPerCu));
        metric += suTilesPerCuDim;

        double summationPerformanceDim
            = abs(ppReference.summationPerformance - pp.summationPerformance);
        metric += summationPerformanceDim;

        return metric;
    }

    double ContractionSolution::computeTAMScore(Problem const&  problem,
                                                Hardware const& hardware,
                                                double          model_M,
                                                double          model_N,
                                                double          model_K,
                                                double          model_NumBatches) const
    {
        double M = 1.0, N = 1.0;
        if(problem.freeIndicesA().size() > 1 || sizeMapping.packBatchDims & 0x1)
        {
            std::vector<size_t> packedIndices
                = generatePackedIndicesA(problem, sizeMapping.packBatchDims);
            for(auto pi = packedIndices.begin(); pi != packedIndices.end(); pi++)
                M *= problem.a().sizes()[*pi];
        }
        else
            M = problem.freeSizeA(0);

        if(problem.freeIndicesB().size() > 1 || sizeMapping.packBatchDims & 0x2)
        {
            std::vector<size_t> packedIndices
                = generatePackedIndicesB(problem, sizeMapping.packBatchDims);
            for(auto pi = packedIndices.begin(); pi != packedIndices.end(); pi++)
                N *= problem.b().sizes()[*pi];
        }
        else
            N = problem.freeSizeB(0);

        double NumBatches = 1;
        if(sizeMapping.packBatchDims == 0)
        {
            for(size_t i = 0; i < problem.batchIndices().size(); i++)
                NumBatches *= problem.batchSize(i);
        }
        double K = problem.boundSize(0); // TODO - fix for multiple summations

        ContractionSolution::TAMetricProblemScore pp
            = computeProblemScore(hardware, M, N, K, NumBatches);

        ContractionSolution::TAMetricProblemScore ppReference
            = computeProblemScore(hardware, model_M, model_N, model_K, model_NumBatches);

        double distance = computeTileAwareMetric(pp, ppReference);

        return distance;
    }

    std::ostream& operator<<(std::ostream&                                      stream,
                             ContractionSolution::StaticPerformanceModel const& spm)
    {
        return stream << " memReadBytesA=" << spm.memReadBytesA
                      << " memReadBytesB=" << spm.memReadBytesB
                      << " memReadBytesC=" << spm.memReadBytesC
                      << " memWriteBytesD=" << spm.memWriteBytesD;
    }

    std::ostream& operator<<(std::ostream&                                    stream,
                             ContractionSolution::ProjectedPerformance const& pp)
    {
        return stream << " numTiles0=" << pp.granularities.numTiles0
                      << " numTiles1=" << pp.granularities.numTiles1
                      << " tilesPerCu=" << pp.granularities.tilesPerCu

                      << " totalGranularity=" << pp.granularities.totalGranularity
                      << " tile0Granularity=" << pp.granularities.tile0Granularity
                      << " tile1Granularity=" << pp.granularities.tile1Granularity
                      << " cuGranularity=" << pp.granularities.cuGranularity
                      << " waveGranularity=" << pp.granularities.waveGranularity

                      << " speedGFlops=" << pp.speedGFlops

                      << " staticModel=[ " << pp.staticModel << " ]";
    }

    std::ostream& operator<<(std::ostream& stream, BufferLoadCheckPacket const& st)
    {
        return stream << " shiftPtrElemA=" << st.shiftPtrElemA
                      << " shiftPtrElemB=" << st.shiftPtrElemB << " depthUorMT0=" << st.depthUorMT0
                      << " depthUorMT1=" << st.depthUorMT1;
    }
} // namespace Tensile