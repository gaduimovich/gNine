#include "VectorProgram.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace gnine
{
   namespace
   {
      struct StageProgram
      {
         std::vector<std::string> argNames;
         std::map<std::string, Cell> defines;
         Cell resultExpr;
      };

      struct NormalizedValue;

      struct LambdaValue
      {
         std::vector<std::string> params;
         Cell body;
      };

      struct NormalizedValue
      {
         bool isLambda;
         Cell expr;
         LambdaValue lambda;
      };

      typedef std::map<std::string, NormalizedValue> NormalizeEnv;

      struct PipelineBinding
      {
         bool isPreviousOutput;
         std::string externalName;
      };

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

      NormalizedValue makeExprValue(const Cell &cell)
      {
         NormalizedValue value;
         value.isLambda = false;
         value.expr = cell;
         return value;
      }

      NormalizedValue makeLambdaValue(const LambdaValue &lambda)
      {
         NormalizedValue value;
         value.isLambda = true;
         value.lambda = lambda;
         return value;
      }

      bool isLambdaForm(const Cell &cell)
      {
         return cell.type == Cell::List &&
                cell.list.size() == 3 &&
                cell.list[0].type == Cell::Symbol &&
                cell.list[0].val == "lambda" &&
                cell.list[1].type == Cell::List;
      }

      NormalizedValue normalizeExpr(const Cell &cell, const NormalizeEnv &env);

      Cell requireExpr(const NormalizedValue &value, const std::string &context)
      {
         if (value.isLambda)
            throw std::runtime_error(context + " cannot use a lambda as a scalar/image expression");
         return value.expr;
      }

      std::vector<Cell> requireExprArgs(const std::vector<NormalizedValue> &args, const std::string &context)
      {
         std::vector<Cell> exprArgs;
         exprArgs.reserve(args.size());
         for (size_t idx = 0; idx < args.size(); ++idx)
            exprArgs.push_back(requireExpr(args[idx], context));
         return exprArgs;
      }

      NormalizedValue applyCallable(const NormalizedValue &callable,
                                    const std::vector<NormalizedValue> &args,
                                    const std::string &context)
      {
         if (callable.isLambda)
         {
            const LambdaValue &lambda = callable.lambda;
            if (lambda.params.size() != args.size())
               throw std::runtime_error(context + " expected " + std::to_string(lambda.params.size()) +
                                        " arguments but got " + std::to_string(args.size()));

            NormalizeEnv callEnv;
            for (size_t idx = 0; idx < args.size(); ++idx)
               callEnv[lambda.params[idx]] = args[idx];
            return normalizeExpr(lambda.body, callEnv);
         }

         if (callable.expr.type != Cell::Symbol)
            throw std::runtime_error(context + " requires a lambda or symbol in function position");

         return makeExprValue(makeCall(callable.expr.val, requireExprArgs(args, context)));
      }

      NormalizedValue normalizeExpr(const Cell &cell, const NormalizeEnv &env)
      {
         if (cell.type == Cell::Number)
            return makeExprValue(cell);

         if (cell.type == Cell::Symbol)
         {
            NormalizeEnv::const_iterator bindingIt = env.find(cell.val);
            if (bindingIt != env.end())
               return bindingIt->second;
            return makeExprValue(cell);
         }

         if (cell.type != Cell::List || cell.list.empty())
            throw std::runtime_error("Invalid expression");

         if (isLambdaForm(cell))
         {
            LambdaValue lambda;
            std::set<std::string> seenParams;
            NormalizeEnv lambdaEnv = env;

            for (size_t idx = 0; idx < cell.list[1].list.size(); ++idx)
            {
               const Cell &param = cell.list[1].list[idx];
               if (param.type != Cell::Symbol)
                  throw std::runtime_error("lambda parameters must be symbols");
               if (!seenParams.insert(param.val).second)
                  throw std::runtime_error("lambda parameters must be unique");
               lambda.params.push_back(param.val);
               lambdaEnv.erase(param.val);
            }

            lambda.body = requireExpr(normalizeExpr(cell.list[2], lambdaEnv), "lambda body");
            return makeLambdaValue(lambda);
         }

         const std::string context = "call to " +
                                     (cell.list[0].type == Cell::Symbol ? cell.list[0].val : std::string("<lambda>"));

         if (cell.list[0].type == Cell::Symbol &&
             (cell.list[0].val == "map-image" || cell.list[0].val == "zip-image"))
         {
            const std::string &name = cell.list[0].val;
            size_t expectedArgs = name == "map-image" ? 2 : 3;
            if (cell.list.size() != expectedArgs + 1)
               throw std::runtime_error(name + " expects " + std::to_string(expectedArgs) + " arguments");

            NormalizedValue callable = normalizeExpr(cell.list[1], env);
            std::vector<NormalizedValue> args;
            for (size_t idx = 2; idx < cell.list.size(); ++idx)
               args.push_back(normalizeExpr(cell.list[idx], env));
            return applyCallable(callable, args, name);
         }

         NormalizedValue callable = normalizeExpr(cell.list[0], env);
         std::vector<NormalizedValue> args;
         args.reserve(cell.list.size() - 1);
         for (size_t idx = 1; idx < cell.list.size(); ++idx)
            args.push_back(normalizeExpr(cell.list[idx], env));
         return applyCallable(callable, args, context);
      }

      Cell normalizeProgram(const Cell &program)
      {
         runtime::Evaluator evaluator;
         return evaluator.normalizeProgram(program);
      }

      bool isPipelineForm(const Cell &cell)
      {
         return cell.type == Cell::List &&
                cell.list.size() >= 2 &&
                cell.list[0].type == Cell::Symbol &&
                cell.list[0].val == "pipeline";
      }

      bool containsPipelineVectorForms(const Cell &cell)
      {
         if (cell.type != Cell::List)
            return false;

         if (!cell.list.empty() && cell.list[0].type == Cell::Symbol)
         {
            const std::string &head = cell.list[0].val;
            if (head == "vec" || head == "rgb" || head == "color" || head == "r" ||
                head == "g" || head == "b" || head == "dot")
               return true;
         }

         for (size_t idx = 0; idx < cell.list.size(); ++idx)
         {
            if (containsPipelineVectorForms(cell.list[idx]))
               return true;
         }

         return false;
      }

      StageProgram parseStageProgram(const Cell &program, const std::string &errorPrefix)
      {
         if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
            throw std::runtime_error(errorPrefix + " must be of form ((A ...) expr)");

         StageProgram stage;
         const Cell &argsCell = program.list[0];
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         {
            if (argsCell.list[idx].type != Cell::Symbol)
               throw std::runtime_error(errorPrefix + " arguments must be symbols");
            stage.argNames.push_back(argsCell.list[idx].val);
         }

         bool sawResult = false;
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
               stage.defines[expr.list[1].val] = expr.list[2];
            }
            else
            {
               if (sawResult)
                  throw std::runtime_error(errorPrefix + " supports a single result expression plus defines");
               stage.resultExpr = expr;
               sawResult = true;
            }
         }

         if (!sawResult)
            throw std::runtime_error(errorPrefix + " must contain a result expression");

         return stage;
      }

      Cell combineCoordinate(const Cell &base, const Cell &offset)
      {
         if (offset.type == Cell::Number && offset.val == "0")
            return base;
         return makeCall("+", {base, offset});
      }

      Cell rebaseComposedExpr(const Cell &expr,
                              const std::set<std::string> &externalArgs,
                              const Cell &rowCoord,
                              const Cell &colCoord);

      Cell substitutePipelineExpr(const Cell &expr,
                                  const StageProgram &stage,
                                  const std::map<std::string, PipelineBinding> &bindings,
                                  const std::set<std::string> &externalArgs,
                                  const Cell *previousExpr,
                                  const Cell &rowCoord,
                                  const Cell &colCoord)
      {
         if (expr.type == Cell::Number)
            return expr;

         if (expr.type == Cell::Symbol)
         {
            std::map<std::string, Cell>::const_iterator defineIt = stage.defines.find(expr.val);
            if (defineIt != stage.defines.end())
               return substitutePipelineExpr(defineIt->second, stage, bindings, externalArgs, previousExpr, rowCoord, colCoord);

            if (expr.val == "i")
               return rowCoord;
            if (expr.val == "j")
               return colCoord;

            std::map<std::string, PipelineBinding>::const_iterator bindingIt = bindings.find(expr.val);
            if (bindingIt != bindings.end())
            {
               if (bindingIt->second.isPreviousOutput)
               {
                  if (previousExpr == NULL)
                     throw std::runtime_error("internal pipeline previous-expression error");
                  return rebaseComposedExpr(*previousExpr, externalArgs, rowCoord, colCoord);
               }

               return makeCall("@" + bindingIt->second.externalName, {rowCoord, colCoord});
            }

            return expr;
         }

         if (expr.type != Cell::List || expr.list.empty() || expr.list[0].type != Cell::Symbol)
            return expr;

         const std::string &head = expr.list[0].val;
         bool isAbsoluteSample = !head.empty() && head[0] == '@';
         std::string bindingName = isAbsoluteSample ? head.substr(1) : head;
         std::map<std::string, PipelineBinding>::const_iterator bindingIt = bindings.find(bindingName);
         if (bindingIt != bindings.end() && expr.list.size() == 3)
         {
            Cell sampleRow = substitutePipelineExpr(expr.list[1], stage, bindings, externalArgs, previousExpr, rowCoord, colCoord);
            Cell sampleCol = substitutePipelineExpr(expr.list[2], stage, bindings, externalArgs, previousExpr, rowCoord, colCoord);
            if (!isAbsoluteSample)
            {
               sampleRow = combineCoordinate(rowCoord, sampleRow);
               sampleCol = combineCoordinate(colCoord, sampleCol);
            }

            if (bindingIt->second.isPreviousOutput)
            {
               if (previousExpr == NULL)
                  throw std::runtime_error("internal pipeline previous-expression error");
               return rebaseComposedExpr(*previousExpr, externalArgs, sampleRow, sampleCol);
            }

            return makeCall("@" + bindingIt->second.externalName, {sampleRow, sampleCol});
         }

         std::vector<Cell> items;
         items.reserve(expr.list.size());
         items.push_back(expr.list[0]);
         for (size_t idx = 1; idx < expr.list.size(); ++idx)
            items.push_back(substitutePipelineExpr(expr.list[idx], stage, bindings, externalArgs, previousExpr, rowCoord, colCoord));
         return makeList(items);
      }

      Cell rebaseComposedExpr(const Cell &expr,
                              const std::set<std::string> &externalArgs,
                              const Cell &rowCoord,
                              const Cell &colCoord)
      {
         if (expr.type == Cell::Number)
            return expr;

         if (expr.type == Cell::Symbol)
         {
            if (expr.val == "i")
               return rowCoord;
            if (expr.val == "j")
               return colCoord;
            if (expr.val == "__pipeline_previous__")
               throw std::runtime_error("internal pipeline rebasing error");
            if (externalArgs.count(expr.val) > 0)
               return makeCall("@" + expr.val, {rowCoord, colCoord});
            return expr;
         }

         if (expr.type != Cell::List || expr.list.empty() || expr.list[0].type != Cell::Symbol)
            return expr;

         const std::string &head = expr.list[0].val;
         bool isAbsoluteSample = !head.empty() && head[0] == '@';
         std::string imageName = isAbsoluteSample ? head.substr(1) : head;
         if (externalArgs.count(imageName) > 0 && expr.list.size() == 3)
         {
            Cell sampleRow = rebaseComposedExpr(expr.list[1], externalArgs, rowCoord, colCoord);
            Cell sampleCol = rebaseComposedExpr(expr.list[2], externalArgs, rowCoord, colCoord);
            if (!isAbsoluteSample)
            {
               sampleRow = combineCoordinate(rowCoord, sampleRow);
               sampleCol = combineCoordinate(colCoord, sampleCol);
            }
            return makeCall("@" + imageName, {sampleRow, sampleCol});
         }

         std::vector<Cell> items;
         items.reserve(expr.list.size());
         items.push_back(expr.list[0]);
         for (size_t idx = 1; idx < expr.list.size(); ++idx)
            items.push_back(rebaseComposedExpr(expr.list[idx], externalArgs, rowCoord, colCoord));
         return makeList(items);
      }

      Cell expandPipelineProgram(const Cell &program)
      {
         if (!isPipelineForm(program))
            return program;

         std::vector<StageProgram> stages;
         for (size_t idx = 1; idx < program.list.size(); ++idx)
         {
            if (containsPipelineVectorForms(program.list[idx]))
               throw std::runtime_error("pipeline currently supports scalar stages only");
            stages.push_back(parseStageProgram(program.list[idx], "Pipeline stage"));
         }

         if (stages.empty())
            throw std::runtime_error("pipeline requires at least one stage");

         const std::vector<std::string> &externalArgList = stages[0].argNames;
         std::set<std::string> externalArgs(externalArgList.begin(), externalArgList.end());

         std::map<std::string, PipelineBinding> firstStageBindings;
         for (size_t idx = 0; idx < externalArgList.size(); ++idx)
         {
            PipelineBinding binding;
            binding.isPreviousOutput = false;
            binding.externalName = externalArgList[idx];
            firstStageBindings[externalArgList[idx]] = binding;
         }

         Cell currentExpr = substitutePipelineExpr(stages[0].resultExpr,
                                                   stages[0],
                                                   firstStageBindings,
                                                   externalArgs,
                                                   NULL,
                                                   makeSymbol("i"),
                                                   makeSymbol("j"));

         for (size_t stageIdx = 1; stageIdx < stages.size(); ++stageIdx)
         {
            const StageProgram &stage = stages[stageIdx];
            if (stage.argNames.empty())
               throw std::runtime_error("Pipeline stages after the first must accept the previous output as their first argument");

            std::map<std::string, PipelineBinding> stageBindings;
            PipelineBinding previous;
            previous.isPreviousOutput = true;
            stageBindings[stage.argNames[0]] = previous;

            for (size_t argIdx = 1; argIdx < stage.argNames.size(); ++argIdx)
            {
               const std::string &argName = stage.argNames[argIdx];
               if (externalArgs.count(argName) == 0)
                  throw std::runtime_error("Pipeline stage argument " + argName + " does not match an external input");

               PipelineBinding binding;
               binding.isPreviousOutput = false;
               binding.externalName = argName;
               stageBindings[argName] = binding;
            }

            Cell stageExpr = substitutePipelineExpr(stage.resultExpr,
                                                    stage,
                                                    stageBindings,
                                                    externalArgs,
                                                    &currentExpr,
                                                    makeSymbol("i"),
                                                    makeSymbol("j"));
            currentExpr = rebaseComposedExpr(stageExpr, externalArgs, makeSymbol("i"), makeSymbol("j"));
         }

         Cell argsCell(Cell::List);
         for (size_t idx = 0; idx < externalArgList.size(); ++idx)
            argsCell.list.push_back(makeSymbol(externalArgList[idx]));

         Cell fusedProgram(Cell::List);
         fusedProgram.list.push_back(argsCell);
         fusedProgram.list.push_back(currentExpr);
         return fusedProgram;
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
      Cell effectiveProgram = normalizeProgram(expandPipelineProgram(program));

      if (effectiveProgram.type != Cell::List || effectiveProgram.list.empty() || effectiveProgram.list[0].type != Cell::List)
         throw std::runtime_error("Program must be of form ((A ...) expr)");

      LowerContext ctx;
      ctx.usedVectorFeatures = false;
      ctx.usedScalarImageSyntax = false;

      const Cell &argsCell = effectiveProgram.list[0];
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
      for (size_t idx = 1; idx < effectiveProgram.list.size(); ++idx)
      {
         const Cell &expr = effectiveProgram.list[idx];
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
         loweredProgram.channelPrograms.push_back(effectiveProgram);
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
