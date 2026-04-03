#include "VectorProgram.hpp"

#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace gnine
{
   namespace
   {
      enum class LoweredKind
      {
         Scalar,
         Vector
      };

      struct LoweredExpr
      {
         LoweredKind kind;
         Cell scalar;
         std::array<Cell, 3> vector;
      };

      struct LowerContext
      {
         std::set<std::string> imageArgs;
         std::map<std::string, LoweredKind> locals;
         bool usedVectorFeatures;
         bool usedScalarImageSyntax;
      };

      Cell makeSymbol(const std::string &value)
      {
         return Cell(Cell::Symbol, value);
      }

      Cell makeNumber(const std::string &value)
      {
         return Cell(Cell::Number, value);
      }

      Cell makeList(const std::vector<Cell> &items)
      {
         Cell cell(Cell::List);
         cell.list = items;
         return cell;
      }

      Cell makeCall(const std::string &head, const std::vector<Cell> &args)
      {
         std::vector<Cell> items;
         items.push_back(makeSymbol(head));
         items.insert(items.end(), args.begin(), args.end());
         return makeList(items);
      }

      std::string channelName(const std::string &base, int channel)
      {
         static const char *suffixes[] = {"__r", "__g", "__b"};
         return base + suffixes[channel];
      }

      LoweredExpr makeScalarExpr(const Cell &cell)
      {
         LoweredExpr result;
         result.kind = LoweredKind::Scalar;
         result.scalar = cell;
         return result;
      }

      LoweredExpr makeVectorExpr(const std::array<Cell, 3> &cells)
      {
         LoweredExpr result;
         result.kind = LoweredKind::Vector;
         result.vector = cells;
         return result;
      }

      bool isComponentwiseFunction(const std::string &name)
      {
         return name == "+" || name == "-" || name == "*" || name == "/" ||
                name == "min" || name == "max" || name == "abs" || name == "clamp";
      }

      LoweredExpr promoteToVector(const LoweredExpr &expr)
      {
         if (expr.kind == LoweredKind::Vector)
            return expr;

         std::array<Cell, 3> channels = {expr.scalar, expr.scalar, expr.scalar};
         return makeVectorExpr(channels);
      }

      Cell lowerScalarOnly(const Cell &cell, LowerContext &ctx);
      LoweredExpr lowerExpr(const Cell &cell, LowerContext &ctx);

      Cell lowerScalarOnly(const Cell &cell, LowerContext &ctx)
      {
         LoweredExpr lowered = lowerExpr(cell, ctx);
         if (lowered.kind != LoweredKind::Scalar)
            throw std::runtime_error("Expected a scalar expression");
         return lowered.scalar;
      }

      LoweredExpr lowerColorSample(const Cell &cell, LowerContext &ctx)
      {
         ctx.usedVectorFeatures = true;
         if (cell.list.size() != 2 && cell.list.size() != 4)
            throw std::runtime_error("color expects (color IMAGE) or (color IMAGE DI DJ)");
         if (cell.list[1].type != Cell::Symbol || ctx.imageArgs.count(cell.list[1].val) == 0)
            throw std::runtime_error("color expects an image argument symbol");

         const std::string &imageName = cell.list[1].val;
         if (cell.list.size() == 2)
         {
            return makeVectorExpr({
                makeSymbol(channelName(imageName, 0)),
                makeSymbol(channelName(imageName, 1)),
                makeSymbol(channelName(imageName, 2)),
            });
         }

         Cell di = lowerScalarOnly(cell.list[2], ctx);
         Cell dj = lowerScalarOnly(cell.list[3], ctx);
         return makeVectorExpr({
             makeCall(channelName(imageName, 0), {di, dj}),
             makeCall(channelName(imageName, 1), {di, dj}),
             makeCall(channelName(imageName, 2), {di, dj}),
         });
      }

      LoweredExpr lowerExpr(const Cell &cell, LowerContext &ctx)
      {
         if (cell.type == Cell::Number)
            return makeScalarExpr(cell);

         if (cell.type == Cell::Symbol)
         {
            std::map<std::string, LoweredKind>::const_iterator localIt = ctx.locals.find(cell.val);
            if (localIt != ctx.locals.end())
            {
               if (localIt->second == LoweredKind::Scalar)
                  return makeScalarExpr(cell);

               return makeVectorExpr({
                   makeSymbol(channelName(cell.val, 0)),
                   makeSymbol(channelName(cell.val, 1)),
                   makeSymbol(channelName(cell.val, 2)),
               });
            }

            if (ctx.imageArgs.count(cell.val) > 0)
               ctx.usedScalarImageSyntax = true;

            return makeScalarExpr(cell);
         }

         if (cell.type != Cell::List || cell.list.empty())
            throw std::runtime_error("Invalid expression");

         if (cell.list[0].type != Cell::Symbol)
            throw std::runtime_error("List head must be a symbol");

         const std::string &head = cell.list[0].val;

         if (head == "vec" || head == "rgb")
         {
            ctx.usedVectorFeatures = true;
            if (cell.list.size() != 4)
               throw std::runtime_error(head + " expects three scalar components");

            std::array<Cell, 3> channels = {
                lowerScalarOnly(cell.list[1], ctx),
                lowerScalarOnly(cell.list[2], ctx),
                lowerScalarOnly(cell.list[3], ctx),
            };
            return makeVectorExpr(channels);
         }

         if (head == "color")
            return lowerColorSample(cell, ctx);

         if (head == "r" || head == "g" || head == "b")
         {
            ctx.usedVectorFeatures = true;
            if (cell.list.size() != 2)
               throw std::runtime_error(head + " expects exactly one vector argument");

            LoweredExpr value = lowerExpr(cell.list[1], ctx);
            if (value.kind != LoweredKind::Vector)
               throw std::runtime_error(head + " expects a vector argument");

            int channel = head == "r" ? 0 : (head == "g" ? 1 : 2);
            return makeScalarExpr(value.vector[channel]);
         }

         if (head == "dot")
         {
            ctx.usedVectorFeatures = true;
            if (cell.list.size() != 3)
               throw std::runtime_error("dot expects exactly two vector arguments");

            LoweredExpr lhs = promoteToVector(lowerExpr(cell.list[1], ctx));
            LoweredExpr rhs = promoteToVector(lowerExpr(cell.list[2], ctx));

            Cell sum = makeCall("+", {
                makeCall("*", {lhs.vector[0], rhs.vector[0]}),
                makeCall("*", {lhs.vector[1], rhs.vector[1]}),
                makeCall("*", {lhs.vector[2], rhs.vector[2]}),
            });
            return makeScalarExpr(sum);
         }

         if (isComponentwiseFunction(head))
         {
            std::vector<LoweredExpr> loweredArgs;
            bool hasVectorArg = false;
            for (size_t idx = 1; idx < cell.list.size(); ++idx)
            {
               loweredArgs.push_back(lowerExpr(cell.list[idx], ctx));
               if (loweredArgs.back().kind == LoweredKind::Vector)
                  hasVectorArg = true;
            }

            if (!hasVectorArg)
            {
               std::vector<Cell> scalarArgs;
               for (size_t idx = 0; idx < loweredArgs.size(); ++idx)
                  scalarArgs.push_back(loweredArgs[idx].scalar);
               return makeScalarExpr(makeCall(head, scalarArgs));
            }

            ctx.usedVectorFeatures = true;
            std::array<Cell, 3> channels;
            for (int channel = 0; channel < 3; ++channel)
            {
               std::vector<Cell> channelArgs;
               for (size_t idx = 0; idx < loweredArgs.size(); ++idx)
               {
                  LoweredExpr promoted = promoteToVector(loweredArgs[idx]);
                  channelArgs.push_back(promoted.vector[channel]);
               }
               channels[channel] = makeCall(head, channelArgs);
            }
            return makeVectorExpr(channels);
         }

         if (ctx.imageArgs.count(head) > 0 || (!head.empty() && head[0] == '@' && ctx.imageArgs.count(head.substr(1)) > 0))
            ctx.usedScalarImageSyntax = true;

         std::vector<Cell> scalarArgs;
         scalarArgs.reserve(cell.list.size() - 1);
         for (size_t idx = 1; idx < cell.list.size(); ++idx)
            scalarArgs.push_back(lowerScalarOnly(cell.list[idx], ctx));
         return makeScalarExpr(makeCall(head, scalarArgs));
      }

      Cell buildProgramCell(const std::vector<std::string> &argNames,
                            const std::vector<Cell> &defines,
                            const Cell &resultExpr)
      {
         Cell argsCell(Cell::List);
         for (size_t idx = 0; idx < argNames.size(); ++idx)
            argsCell.list.push_back(makeSymbol(argNames[idx]));

         Cell program(Cell::List);
         program.list.push_back(argsCell);
         program.list.insert(program.list.end(), defines.begin(), defines.end());
         program.list.push_back(resultExpr);
         return program;
      }
   }

   LoweredProgram lowerProgram(const Cell &program)
   {
      if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
         throw std::runtime_error("Program must be of form ((A ...) expr)");

      LowerContext ctx;
      ctx.usedVectorFeatures = false;
      ctx.usedScalarImageSyntax = false;

      const Cell &argsCell = program.list[0];
      std::vector<std::string> inputNames;
      for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
      {
         if (argsCell.list[idx].type != Cell::Symbol)
            throw std::runtime_error("Program arguments must be symbols");
         inputNames.push_back(argsCell.list[idx].val);
         ctx.imageArgs.insert(argsCell.list[idx].val);
      }

      std::vector<Cell> defineStatements;
      bool sawResult = false;
      LoweredExpr finalExpr;
      for (size_t idx = 1; idx < program.list.size(); ++idx)
      {
         const Cell &expr = program.list[idx];
         bool isDefine = expr.type == Cell::List &&
                         expr.list.size() == 3 &&
                         expr.list[0].type == Cell::Symbol &&
                         expr.list[0].val == "define" &&
                         expr.list[1].type == Cell::Symbol;

         if (isDefine)
         {
            LoweredExpr lowered = lowerExpr(expr.list[2], ctx);
            ctx.locals[expr.list[1].val] = lowered.kind;
            defineStatements.push_back(expr);
         }
         else
         {
            if (sawResult)
               throw std::runtime_error("Vector lowering currently supports a single result expression plus defines");
            finalExpr = lowerExpr(expr, ctx);
            sawResult = true;
         }
      }

      if (!sawResult)
         throw std::runtime_error("Program must contain a result expression");

      LoweredProgram loweredProgram;
      loweredProgram.usesVectorFeatures = ctx.usedVectorFeatures;
      loweredProgram.outputIsVector = finalExpr.kind == LoweredKind::Vector;

      if (!loweredProgram.usesVectorFeatures)
      {
         loweredProgram.channelPrograms.push_back(program);
         for (size_t idx = 0; idx < inputNames.size(); ++idx)
         {
            VectorArgBinding binding;
            binding.inputIndex = idx;
            binding.channel = -1;
            loweredProgram.argBindings.push_back(binding);
         }
         return loweredProgram;
      }

      if (ctx.usedScalarImageSyntax)
         throw std::runtime_error("Vector programs require explicit (color A) sampling instead of scalar image syntax");

      std::vector<std::string> loweredArgs;
      for (size_t inputIdx = 0; inputIdx < inputNames.size(); ++inputIdx)
      {
         for (int channel = 0; channel < 3; ++channel)
         {
            loweredArgs.push_back(channelName(inputNames[inputIdx], channel));
            VectorArgBinding binding;
            binding.inputIndex = inputIdx;
            binding.channel = channel;
            loweredProgram.argBindings.push_back(binding);
         }
      }

      std::vector<Cell> sharedDefines;
      for (size_t idx = 0; idx < defineStatements.size(); ++idx)
      {
         const Cell &defineExpr = defineStatements[idx];
         LoweredKind kind = ctx.locals[defineExpr.list[1].val];
         if (kind == LoweredKind::Scalar)
         {
            Cell loweredDefine(Cell::List);
            loweredDefine.list.push_back(makeSymbol("define"));
            loweredDefine.list.push_back(makeSymbol(defineExpr.list[1].val));
            loweredDefine.list.push_back(lowerScalarOnly(defineExpr.list[2], ctx));
            sharedDefines.push_back(loweredDefine);
         }
         else
         {
            LoweredExpr rhs = lowerExpr(defineExpr.list[2], ctx);
            for (int channel = 0; channel < 3; ++channel)
            {
               Cell loweredDefine(Cell::List);
               loweredDefine.list.push_back(makeSymbol("define"));
               loweredDefine.list.push_back(makeSymbol(channelName(defineExpr.list[1].val, channel)));
               loweredDefine.list.push_back(rhs.vector[channel]);
               sharedDefines.push_back(loweredDefine);
            }
         }
      }

      if (loweredProgram.outputIsVector)
      {
         for (int channel = 0; channel < 3; ++channel)
            loweredProgram.channelPrograms.push_back(buildProgramCell(loweredArgs, sharedDefines, finalExpr.vector[channel]));
      }
      else
      {
         loweredProgram.channelPrograms.push_back(buildProgramCell(loweredArgs, sharedDefines, finalExpr.scalar));
      }

      return loweredProgram;
   }
}
