#include "Runtime.hpp"

#include "ImageArray.hpp"
#include "VectorProgram.hpp"
#include "JitBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace gnine
{
   namespace runtime
   {
      namespace
      {
         double parseNumberLiteral(const std::string &literal)
         {
            std::stringstream ss(literal);
            double value = 0.0;
            char trailing = '\0';
            ss >> value;
            if (!ss || (ss >> trailing))
               throw std::runtime_error("Invalid numeric literal: " + literal);
            return value;
         }

         int reflectIndex(int index, int limit)
         {
            if (limit <= 0)
               throw std::runtime_error("Cannot sample from an empty image");

            int maxIndex = limit - 1;
            while (index < 0 || index > maxIndex)
            {
               if (index < 0)
                  index = -index;
               if (index > maxIndex)
                  index = 2 * maxIndex + 1 - index;
            }
            return index;
         }

         struct PixelContextScope
         {
            PixelContextScope(bool &hasPixelContext,
                              int &currentRow,
                              int &currentCol,
                              int &currentChannel,
                              int row,
                              int col,
                              int channel)
               : hasContext(hasPixelContext),
                 rowRef(currentRow),
                 colRef(currentCol),
                 channelRef(currentChannel),
                 previousHasContext(hasPixelContext),
                 previousRow(currentRow),
                 previousCol(currentCol),
                 previousChannel(currentChannel)
            {
               hasContext = true;
               rowRef = row;
               colRef = col;
               channelRef = channel;
            }

            ~PixelContextScope()
            {
               hasContext = previousHasContext;
               rowRef = previousRow;
               colRef = previousCol;
               channelRef = previousChannel;
            }

            bool &hasContext;
            int &rowRef;
            int &colRef;
            int &channelRef;
            bool previousHasContext;
            int previousRow;
            int previousCol;
            int previousChannel;
         };

         Cell makeSymbolCell(const std::string &value)
         {
            return Cell(Cell::Symbol, value);
         }

         Cell makeDefineCell(const std::string &name, const Cell &value)
         {
            Cell defineExpr(Cell::List);
            defineExpr.list.push_back(makeSymbolCell("define"));
            defineExpr.list.push_back(makeSymbolCell(name));
            defineExpr.list.push_back(value);
            return defineExpr;
         }

         Cell doubleToCell(double value)
         {
            std::ostringstream out;
            out << std::setprecision(17) << value;
            return Cell(Cell::Number, out.str());
         }

         bool ensureRuntimeJitInitialized()
         {
            static bool initialized = false;
            if (initialized)
               return true;

            static char jitOptions[] = "-Xjit:acceptHugeMethods,enableBasicBlockHoisting,omitFramePointer,useILValidator";
            initialized = initializeJitWithOptions(jitOptions);
            return initialized;
         }
      }

      Value::Value()
         : kind(Nil), number(0.0), object(NULL)
      {
      }

      Value Value::nil()
      {
         return Value();
      }

      Value Value::numberValue(double value)
      {
         Value result;
         result.kind = Number;
         result.number = value;
         return result;
      }

      Value Value::exprValue(const Cell &expr)
      {
         Value result;
         result.kind = Expr;
         result.expr = expr;
         return result;
      }

      Value Value::builtinValue(const std::string &name)
      {
         Value result;
         result.kind = Builtin;
         result.builtinName = name;
         return result;
      }

      Value Value::objectValue(Object *value)
      {
         Value result;
         result.kind = ObjectRef;
         result.object = value;
         return result;
      }

      bool Value::isNil() const
      {
         return kind == Nil;
      }

      bool Value::isNumber() const
      {
         return kind == Number;
      }

      bool Value::isExpr() const
      {
         return kind == Expr;
      }

      bool Value::isBuiltin() const
      {
         return kind == Builtin;
      }

      bool Value::isObject() const
      {
         return kind == ObjectRef;
      }

      Object::Object(Type objectType)
         : type(objectType), marked(false), next(NULL)
      {
      }

      Object::~Object()
      {
      }

      EnvironmentObject::EnvironmentObject(EnvironmentObject *parentEnv)
         : Object(Environment), parent(parentEnv)
      {
      }

      void EnvironmentObject::trace(Heap &heap)
      {
         heap.markObject(parent);
         for (std::map<std::string, Value>::const_iterator it = bindings.begin();
              it != bindings.end();
              ++it)
            heap.markValue(it->second);
      }

      ClosureObject::ClosureObject(const std::vector<std::string> &parameters,
                                   const Cell &bodyExpr,
                                   EnvironmentObject *capturedEnv)
         : Object(Closure), params(parameters), body(bodyExpr), env(capturedEnv)
      {
      }

      void ClosureObject::trace(Heap &heap)
      {
         heap.markObject(env);
      }

      ImageObject::ImageObject(const gnine::Image &source)
         : Object(Image), image(new gnine::Image(source.width(), source.height(), source.stride(), source.channelCount()))
      {
         std::copy(source.getData(),
                   source.getData() + source.channelCount() * source.planeSize(),
                   image->getData());
      }

      ImageObject::~ImageObject()
      {
         delete image;
      }

      void ImageObject::trace(Heap &)
      {
      }

      Heap::Root::Root(Heap &heap, Value &slot)
         : _heap(&heap), _slot(&slot)
      {
         _heap->addRoot(_slot);
      }

      Heap::Root::~Root()
      {
         _heap->removeRoot(_slot);
      }

      Heap::Heap()
         : _objects(NULL),
           _liveObjects(0),
           _collectionCount(0),
           _nextCollectionThreshold(16)
      {
      }

      Heap::~Heap()
      {
         Object *object = _objects;
         while (object != NULL)
         {
            Object *next = object->next;
            delete object;
            object = next;
         }
      }

      EnvironmentObject *Heap::allocateEnvironment(EnvironmentObject *parent)
      {
         return allocateObject<EnvironmentObject>(parent);
      }

      ClosureObject *Heap::allocateClosure(const std::vector<std::string> &params,
                                           const Cell &body,
                                           EnvironmentObject *env)
      {
         return allocateObject<ClosureObject>(params, body, env);
      }

      ImageObject *Heap::allocateImage(const gnine::Image &image)
      {
         return allocateObject<ImageObject>(image);
      }

      void Heap::addRoot(Value *slot)
      {
         _roots.push_back(slot);
      }

      void Heap::removeRoot(Value *slot)
      {
         std::vector<Value *>::iterator it = std::find(_roots.begin(), _roots.end(), slot);
         if (it == _roots.end())
            throw std::runtime_error("Attempted to remove unknown GC root");
         _roots.erase(it);
      }

      void Heap::collect()
      {
         ++_collectionCount;
         for (size_t idx = 0; idx < _roots.size(); ++idx)
            markValue(*_roots[idx]);
         sweep();
         _nextCollectionThreshold = std::max(static_cast<size_t>(16), _liveObjects * 2);
      }

      void Heap::markValue(const Value &value)
      {
         if (value.isObject())
            markObject(value.object);
      }

      void Heap::markObject(Object *object)
      {
         if (object == NULL || object->marked)
            return;
         object->marked = true;
         object->trace(*this);
      }

      size_t Heap::liveObjectCount() const
      {
         return _liveObjects;
      }

      size_t Heap::collectionCount() const
      {
         return _collectionCount;
      }

      template <typename T, typename... Args>
      T *Heap::allocateObject(Args&&... args)
      {
         if (_liveObjects >= _nextCollectionThreshold)
            collect();

         T *object = new T(std::forward<Args>(args)...);
         object->next = _objects;
         _objects = object;
         ++_liveObjects;
         return object;
      }

      void Heap::sweep()
      {
         Object **link = &_objects;
         while (*link != NULL)
         {
            Object *object = *link;
            if (!object->marked)
            {
               *link = object->next;
               delete object;
               --_liveObjects;
            }
            else
            {
               object->marked = false;
               link = &object->next;
            }
         }
      }

      Evaluator::Evaluator()
         : _hasPixelContext(false),
           _currentRow(0),
           _currentCol(0),
           _currentChannel(0)
      {
      }

      Value Evaluator::evaluateProgram(const Cell &program)
      {
         std::map<std::string, Value> bindings;
         return evaluateProgram(program, bindings);
      }

      Value Evaluator::evaluateProgram(const Cell &program, const std::map<std::string, Value> &bindings)
      {
         if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
            throw std::runtime_error("Runtime program must be of form (() expr)");

         const Cell &argsCell = program.list[0];
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         {
            if (argsCell.list[idx].type != Cell::Symbol)
               throw std::runtime_error("Runtime program arguments must be symbols");
         }

         std::vector<Value> rootedBindings;
         rootedBindings.reserve(bindings.size());
         for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
            rootedBindings.push_back(it->second);
         for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
            _heap.addRoot(&rootedBindings[idx]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);

         try
         {
            for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
               globalEnv->bindings[it->first] = it->second;

            for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
            {
               const std::string &name = argsCell.list[idx].val;
               std::map<std::string, Value>::const_iterator it = bindings.find(name);
               if (it == bindings.end())
                  throw std::runtime_error("Missing runtime binding for program argument: " + name);
            }

            Value result = Value::nil();
            Heap::Root resultRoot(_heap, result);
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
                  Value defineValue = eval(expr.list[2], globalEnv);
                  globalEnv->bindings[expr.list[1].val] = defineValue;
                  continue;
               }

               if (sawResult)
                  throw std::runtime_error("Runtime evaluator expects a single result expression plus defines");

               result = eval(expr, globalEnv);
               sawResult = true;
            }

            if (!sawResult)
               throw std::runtime_error("Runtime program must contain a result expression");

            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            return result;
         }
         catch (...)
         {
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            throw;
         }
      }

      Value Evaluator::evaluateExpr(const Cell &expr)
      {
         std::map<std::string, Value> bindings;
         return evaluateExpr(expr, bindings);
      }

      Value Evaluator::evaluateExpr(const Cell &expr, const std::map<std::string, Value> &bindings)
      {
         std::vector<Value> rootedBindings;
         rootedBindings.reserve(bindings.size());
         for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
            rootedBindings.push_back(it->second);
         for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
            _heap.addRoot(&rootedBindings[idx]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);
         try
         {
            for (std::map<std::string, Value>::const_iterator it = bindings.begin(); it != bindings.end(); ++it)
               globalEnv->bindings[it->first] = it->second;
            Value result = eval(expr, globalEnv);
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            return result;
         }
         catch (...)
         {
            for (size_t idx = 0; idx < rootedBindings.size(); ++idx)
               _heap.removeRoot(&rootedBindings[idx]);
            throw;
         }
      }

      Cell Evaluator::normalizeProgram(const Cell &program)
      {
         if (program.type != Cell::List || program.list.empty() || program.list[0].type != Cell::List)
            throw std::runtime_error("Program must be of form ((A ...) expr)");

         Cell normalized(Cell::List);
         normalized.list.push_back(program.list[0]);

         Value globalEnvValue = Value::objectValue(_heap.allocateEnvironment(NULL));
         Heap::Root globalRoot(_heap, globalEnvValue);
         EnvironmentObject *globalEnv = static_cast<EnvironmentObject *>(globalEnvValue.object);

         const Cell &argsCell = program.list[0];
         for (size_t idx = 0; idx < argsCell.list.size(); ++idx)
         {
            if (argsCell.list[idx].type != Cell::Symbol)
               throw std::runtime_error("Program arguments must be symbols");
            globalEnv->bindings[argsCell.list[idx].val] = Value::exprValue(argsCell.list[idx]);
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
               Value value = eval(expr.list[2], globalEnv);
               globalEnv->bindings[expr.list[1].val] = value;

               if (value.isObject() || value.isBuiltin())
                  continue;

               Cell defineExpr(Cell::List);
               defineExpr.list.push_back(Cell(Cell::Symbol, "define"));
               defineExpr.list.push_back(expr.list[1]);
               defineExpr.list.push_back(requireExpr(value, "define"));
               normalized.list.push_back(defineExpr);

               globalEnv->bindings[expr.list[1].val] = Value::exprValue(expr.list[1]);
               continue;
            }

            if (sawResult)
               throw std::runtime_error("Program supports a single result expression plus defines");

            normalized.list.push_back(requireExpr(eval(expr, globalEnv), "program result"));
            sawResult = true;
         }

         if (!sawResult)
            throw std::runtime_error("Program must contain a result expression");

         return normalized;
      }

      Cell Evaluator::requireExpr(const Value &value, const std::string &context) const
      {
         if (value.isExpr())
            return value.expr;
         if (value.isNumber())
            return numberToCell(value.number);
         throw std::runtime_error(context + " cannot use a closure as a scalar/image expression");
      }

      Value Evaluator::imageValue(const gnine::Image &image)
      {
         return Value::objectValue(_heap.allocateImage(image));
      }

      Heap &Evaluator::heap()
      {
         return _heap;
      }

      Value Evaluator::eval(const Cell &expr, EnvironmentObject *env)
      {
         if (expr.type == Cell::Number)
            return Value::numberValue(parseNumberLiteral(expr.val));

         if (expr.type == Cell::Symbol)
         {
            Value value;
            if (lookup(env, expr.val, &value))
               return value;
            if (isBuiltin(expr.val))
               return Value::builtinValue(expr.val);
            return Value::exprValue(expr);
         }

         if (expr.type != Cell::List || expr.list.empty())
            throw std::runtime_error("Invalid runtime expression");

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "lambda")
         {
            if (expr.list.size() != 3 || expr.list[1].type != Cell::List)
               throw std::runtime_error("lambda must be of form (lambda (x ...) body)");

            std::vector<std::string> params;
            for (size_t idx = 0; idx < expr.list[1].list.size(); ++idx)
            {
               const Cell &param = expr.list[1].list[idx];
               if (param.type != Cell::Symbol)
                  throw std::runtime_error("lambda parameters must be symbols");
               params.push_back(param.val);
            }
            return Value::objectValue(_heap.allocateClosure(params, expr.list[2], env));
         }

         if (expr.list[0].type == Cell::Symbol &&
             (expr.list[0].val == "map-image" || expr.list[0].val == "zip-image"))
         {
            const std::string &name = expr.list[0].val;
            size_t imageArgCount = name == "map-image" ? 1 : 2;
            if (expr.list.size() != imageArgCount + 2)
               throw std::runtime_error(name + " expects " + std::to_string(imageArgCount + 1) + " arguments");

            std::vector<Value> args(imageArgCount, Value::nil());
            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.addRoot(&args[idx]);

            Value result = Value::nil();
            Heap::Root resultRoot(_heap, result);

            try
            {
               Value callee = eval(expr.list[1], env);
               Heap::Root calleeRoot(_heap, callee);
               for (size_t idx = 0; idx < args.size(); ++idx)
                  args[idx] = eval(expr.list[idx + 2], env);
               bool allImages = true;
               for (size_t idx = 0; idx < args.size(); ++idx)
               {
                  if (!args[idx].isObject() || args[idx].object->type != Object::Image)
                  {
                     allImages = false;
                     break;
                  }
               }

               if (allImages)
                  result = name == "map-image" ? applyMapImage(callee, args, name) : applyZipImage(callee, args, name);
               else
                  result = applyCallable(callee, args, name);
            }
            catch (...)
            {
               for (size_t idx = 0; idx < args.size(); ++idx)
                  _heap.removeRoot(&args[idx]);
               throw;
            }

            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.removeRoot(&args[idx]);
            return result;
         }

         if (expr.list[0].type == Cell::Symbol && expr.list[0].val == "if")
         {
            if (expr.list.size() != 4)
               throw std::runtime_error("if expects exactly three arguments");

            Value condition = eval(expr.list[1], env);
            Heap::Root conditionRoot(_heap, condition);
            if (condition.isNumber())
               return eval(condition.number != 0.0 ? expr.list[2] : expr.list[3], env);

            Value thenValue = eval(expr.list[2], env);
            Heap::Root thenRoot(_heap, thenValue);
            Value elseValue = eval(expr.list[3], env);
            Heap::Root elseRoot(_heap, elseValue);

            Cell residual(Cell::List);
            residual.list.push_back(Cell(Cell::Symbol, "if"));
            residual.list.push_back(requireExpr(condition, "if condition"));
            residual.list.push_back(requireExpr(thenValue, "if then-branch"));
            residual.list.push_back(requireExpr(elseValue, "if else-branch"));
            return Value::exprValue(residual);
         }

         std::vector<Value> args(expr.list.size() - 1, Value::nil());
         for (size_t idx = 0; idx < args.size(); ++idx)
            _heap.addRoot(&args[idx]);

         Value result = Value::nil();
         Heap::Root resultRoot(_heap, result);

         try
         {
            for (size_t idx = 1; idx < expr.list.size(); ++idx)
               args[idx - 1] = eval(expr.list[idx], env);
            Value callee = eval(expr.list[0], env);
            Heap::Root calleeRoot(_heap, callee);
            result = applyCallable(callee, args,
                                   expr.list[0].type == Cell::Symbol ? expr.list[0].val : std::string("<lambda>"));
         }
         catch (...)
         {
            for (size_t idx = 0; idx < args.size(); ++idx)
               _heap.removeRoot(&args[idx]);
            throw;
         }

         for (size_t idx = 0; idx < args.size(); ++idx)
            _heap.removeRoot(&args[idx]);
         return result;
      }

      Value Evaluator::applyBuiltin(const std::string &builtinName,
                                    const std::vector<Value> &args)
      {
         if (builtinName == "+")
         {
            double sum = 0.0;
            for (size_t idx = 0; idx < args.size(); ++idx)
               sum += requireNumber(args[idx], "+");
            return Value::numberValue(sum);
         }

         if (builtinName == "*")
         {
            double product = 1.0;
            for (size_t idx = 0; idx < args.size(); ++idx)
               product *= requireNumber(args[idx], "*");
            return Value::numberValue(product);
         }

         if (builtinName == "-")
         {
            if (args.empty())
               throw std::runtime_error("- expects at least one argument");
            double value = requireNumber(args[0], "-");
            if (args.size() == 1)
               return Value::numberValue(-value);
            for (size_t idx = 1; idx < args.size(); ++idx)
               value -= requireNumber(args[idx], "-");
            return Value::numberValue(value);
         }

         if (builtinName == "/")
         {
            if (args.empty())
               throw std::runtime_error("/ expects at least one argument");
            double value = requireNumber(args[0], "/");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value /= requireNumber(args[idx], "/");
            return Value::numberValue(value);
         }

         if (builtinName == "abs")
         {
            if (args.size() != 1)
               throw std::runtime_error("abs expects exactly one argument");
            return Value::numberValue(std::fabs(requireNumber(args[0], "abs")));
         }

         if (builtinName == "min")
         {
            if (args.empty())
               throw std::runtime_error("min expects at least one argument");
            double value = requireNumber(args[0], "min");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value = std::min(value, requireNumber(args[idx], "min"));
            return Value::numberValue(value);
         }

         if (builtinName == "max")
         {
            if (args.empty())
               throw std::runtime_error("max expects at least one argument");
            double value = requireNumber(args[0], "max");
            for (size_t idx = 1; idx < args.size(); ++idx)
               value = std::max(value, requireNumber(args[idx], "max"));
            return Value::numberValue(value);
         }

         if (builtinName == "clamp")
         {
            if (args.size() != 3)
               throw std::runtime_error("clamp expects exactly three arguments");
            double value = requireNumber(args[0], "clamp");
            double low = requireNumber(args[1], "clamp");
            double high = requireNumber(args[2], "clamp");
            return Value::numberValue(std::min(std::max(value, low), high));
         }

         if (builtinName == "int")
         {
            if (args.size() != 1)
               throw std::runtime_error("int expects exactly one argument");
            return Value::numberValue(static_cast<long long>(requireNumber(args[0], "int")));
         }

         if (builtinName == "<")
         {
            if (args.size() != 2)
               throw std::runtime_error("< expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "<") < requireNumber(args[1], "<") ? 1.0 : 0.0);
         }

         if (builtinName == "<=")
         {
            if (args.size() != 2)
               throw std::runtime_error("<= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "<=") <= requireNumber(args[1], "<=") ? 1.0 : 0.0);
         }

         if (builtinName == ">")
         {
            if (args.size() != 2)
               throw std::runtime_error("> expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], ">") > requireNumber(args[1], ">") ? 1.0 : 0.0);
         }

         if (builtinName == ">=")
         {
            if (args.size() != 2)
               throw std::runtime_error(">= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], ">=") >= requireNumber(args[1], ">=") ? 1.0 : 0.0);
         }

         if (builtinName == "==")
         {
            if (args.size() != 2)
               throw std::runtime_error("== expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "==") == requireNumber(args[1], "==") ? 1.0 : 0.0);
         }

         if (builtinName == "!=")
         {
            if (args.size() != 2)
               throw std::runtime_error("!= expects exactly two arguments");
            return Value::numberValue(requireNumber(args[0], "!=") != requireNumber(args[1], "!=") ? 1.0 : 0.0);
         }

         if (builtinName == "and")
         {
            if (args.empty())
               throw std::runtime_error("and expects at least one argument");
            for (size_t idx = 0; idx < args.size(); ++idx)
            {
               if (requireNumber(args[idx], "and") == 0.0)
                  return Value::numberValue(0.0);
            }
            return Value::numberValue(1.0);
         }

         if (builtinName == "or")
         {
            if (args.empty())
               throw std::runtime_error("or expects at least one argument");
            for (size_t idx = 0; idx < args.size(); ++idx)
            {
               if (requireNumber(args[idx], "or") != 0.0)
                  return Value::numberValue(1.0);
            }
            return Value::numberValue(0.0);
         }

         if (builtinName == "not")
         {
            if (args.size() != 1)
               throw std::runtime_error("not expects exactly one argument");
            return Value::numberValue(requireNumber(args[0], "not") == 0.0 ? 1.0 : 0.0);
         }

         if (builtinName == "width")
         {
            if (args.size() != 1)
               throw std::runtime_error("width expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "width")->image->width());
         }

         if (builtinName == "height")
         {
            if (args.size() != 1)
               throw std::runtime_error("height expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "height")->image->height());
         }

         if (builtinName == "channels")
         {
            if (args.size() != 1)
               throw std::runtime_error("channels expects exactly one argument");
            return Value::numberValue(requireImageObject(args[0], "channels")->image->channelCount());
         }

         throw std::runtime_error("Unsupported runtime builtin: " + builtinName);
      }

      Value Evaluator::applyCallable(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (callable.isObject())
         {
            if (callable.object->type == Object::Closure)
               return applyClosure(static_cast<ClosureObject *>(callable.object), args);
            if (callable.object->type == Object::Image)
               return applyImageSample(static_cast<ImageObject *>(callable.object), args, context);
            throw std::runtime_error(context + " cannot call this runtime object");
         }

         if (callable.isBuiltin())
        {
            if (allConcreteBuiltinArgs(args))
               return applyBuiltin(callable.builtinName, args);

            Value result = Value::nil();
            if (callable.builtinName == "if")
               throw std::runtime_error("if is a special form and cannot be passed as a function");

            if (!allNumbers(args))
            {
               Cell expr(Cell::List);
               expr.list.push_back(Cell(Cell::Symbol, callable.builtinName));
               std::vector<Cell> exprArgs = requireExprArgs(args, context);
               expr.list.insert(expr.list.end(), exprArgs.begin(), exprArgs.end());
               result = Value::exprValue(expr);
            }

            if (!result.isNil())
               return result;
         }

         if (callable.isExpr())
         {
            if (callable.expr.type != Cell::Symbol)
               throw std::runtime_error(context + " requires a lambda or symbol in function position");

            Cell expr(Cell::List);
            expr.list.push_back(callable.expr);
            std::vector<Cell> exprArgs = requireExprArgs(args, context);
            expr.list.insert(expr.list.end(), exprArgs.begin(), exprArgs.end());
            return Value::exprValue(expr);
         }

         throw std::runtime_error(context + " call target is not callable");
      }

      Value Evaluator::applyClosure(ClosureObject *closure,
                                    const std::vector<Value> &args)
      {
         if (closure->params.size() != args.size())
            throw std::runtime_error("Closure expected " + std::to_string(closure->params.size()) +
                                     " arguments but got " + std::to_string(args.size()));

         Value callEnvValue = Value::objectValue(_heap.allocateEnvironment(closure->env));
         Heap::Root callEnvRoot(_heap, callEnvValue);
         EnvironmentObject *callEnv = static_cast<EnvironmentObject *>(callEnvValue.object);

         for (size_t idx = 0; idx < args.size(); ++idx)
            callEnv->bindings[closure->params[idx]] = args[idx];
         if (_hasPixelContext)
         {
            callEnv->bindings["i"] = Value::numberValue(_currentRow);
            callEnv->bindings["j"] = Value::numberValue(_currentCol);
            callEnv->bindings["c"] = Value::numberValue(_currentChannel);
         }

         return eval(closure->body, callEnv);
      }

      Value Evaluator::applyImageSample(ImageObject *image,
                                        const std::vector<Value> &args,
                                        const std::string &context)
      {
         if (!_hasPixelContext)
            throw std::runtime_error(context + " image sampling requires map-image or zip-image pixel context");
         if (args.size() != 2)
            throw std::runtime_error(context + " image sampling expects exactly two offset arguments");

         int row = _currentRow + static_cast<int>(requireNumber(args[0], context));
         int col = _currentCol + static_cast<int>(requireNumber(args[1], context));
         row = reflectIndex(row, image->image->height());
         col = reflectIndex(col, image->image->width());
         int channel = image->image->channelCount() == 1 ? 0 : _currentChannel;
         return Value::numberValue(image->image->operator()(row, col, channel));
      }

      bool Evaluator::lookup(EnvironmentObject *env,
                             const std::string &name,
                             Value *outValue) const
      {
         for (EnvironmentObject *current = env; current != NULL; current = current->parent)
         {
            std::map<std::string, Value>::const_iterator it = current->bindings.find(name);
            if (it != current->bindings.end())
            {
               *outValue = it->second;
               return true;
            }
         }
         return false;
      }

      bool Evaluator::isBuiltin(const std::string &name) const
      {
         return name == "+" || name == "-" || name == "*" || name == "/" ||
                name == "<" || name == "<=" || name == ">" || name == ">=" ||
                name == "==" || name == "!=" || name == "and" || name == "or" ||
                name == "not" || name == "min" || name == "max" || name == "abs" ||
                name == "clamp" || name == "int" ||
                name == "width" || name == "height" || name == "channels";
      }

      bool Evaluator::allNumbers(const std::vector<Value> &args) const
      {
         for (size_t idx = 0; idx < args.size(); ++idx)
         {
            if (!args[idx].isNumber())
               return false;
         }
         return true;
      }

      bool Evaluator::allConcreteBuiltinArgs(const std::vector<Value> &args) const
      {
         for (size_t idx = 0; idx < args.size(); ++idx)
         {
            if (args[idx].isExpr())
               return false;
            if (args[idx].isObject() && args[idx].object->type == Object::Closure)
               return false;
         }
         return true;
      }

      double Evaluator::requireNumber(const Value &value,
                                      const std::string &context) const
      {
         if (!value.isNumber())
            throw std::runtime_error(context + " expects numeric arguments");
         return value.number;
      }

      ImageObject *Evaluator::requireImageObject(const Value &value,
                                                 const std::string &context) const
      {
         if (!value.isObject() || value.object->type != Object::Image)
            throw std::runtime_error(context + " expects image arguments");
         return static_cast<ImageObject *>(value.object);
      }

      bool Evaluator::tryCompiledMapImage(ClosureObject *closure,
                                          const gnine::Image &source,
                                          Value *outResult)
      {
         if (closure->params.size() != 1)
            return false;

         const std::string sourceArgName = "__runtime_source__";
         Cell argsCell(Cell::List);
         argsCell.list.push_back(makeSymbolCell(sourceArgName));

         Cell program(Cell::List);
         program.list.push_back(argsCell);
         if (closure->params[0] != sourceArgName)
            program.list.push_back(makeDefineCell(closure->params[0], makeSymbolCell(sourceArgName)));

         std::set<std::string> seenBindings;
         std::vector<const gnine::Image *> inputImages;
         inputImages.push_back(&source);
         for (EnvironmentObject *current = closure->env; current != NULL; current = current->parent)
         {
            for (std::map<std::string, Value>::const_iterator it = current->bindings.begin();
                 it != current->bindings.end();
                 ++it)
            {
               if (!seenBindings.insert(it->first).second || it->first == closure->params[0])
                  continue;

               const Value &value = it->second;
               if (value.isNumber())
               {
                  program.list.push_back(makeDefineCell(it->first, doubleToCell(value.number)));
                  continue;
               }

               if (value.isExpr())
               {
                  program.list.push_back(makeDefineCell(it->first, value.expr));
                  continue;
               }

               if (value.isBuiltin())
                  continue;

               if (value.isObject() && value.object->type == Object::Image)
               {
                  argsCell.list.push_back(makeSymbolCell(it->first));
                  inputImages.push_back(static_cast<ImageObject *>(value.object)->image);
                  continue;
               }

               return false;
            }
         }

         program.list[0] = argsCell;
         program.list.push_back(closure->body);

         if (!ensureRuntimeJitInitialized())
            throw std::runtime_error("Failed to initialize JIT for compiled runtime map-image");

         try
         {
            LoweredProgram lowered = lowerProgram(program);
            if (lowered.usesVectorFeatures || lowered.channelPrograms.size() != 1)
               return false;

            OMR::JitBuilder::TypeDictionary types;
            ImageArray method(&types);
            method.runByteCodes(lowered.channelPrograms[0], false);

            void *entry = NULL;
            int32_t rc = compileMethodBuilder(&method, &entry);
            if (rc != 0)
               return false;

            ImageArrayFunctionType *fn = reinterpret_cast<ImageArrayFunctionType *>(entry);
            gnine::Image resultImage(source.width(), source.height(), source.stride(), source.channelCount());
            std::vector<double *> dataPtrs;
            dataPtrs.reserve(inputImages.size());
            for (int channel = 0; channel < source.channelCount(); ++channel)
            {
               dataPtrs.clear();
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  int sourceChannel = image->channelCount() == 1 ? 0 : channel;
                  dataPtrs.push_back(const_cast<double *>(image->getChannelData(sourceChannel)));
               }
               fn(source.width(), source.height(), 1, dataPtrs.data(), resultImage.getChannelData(channel));
            }

            *outResult = imageValue(resultImage);
            return true;
         }
         catch (...)
         {
            return false;
         }
      }

      bool Evaluator::tryCompiledZipImage(ClosureObject *closure,
                                          const gnine::Image &lhs,
                                          const gnine::Image &rhs,
                                          Value *outResult)
      {
         if (closure->params.size() != 2)
            return false;

         const std::string lhsArgName = "__runtime_lhs__";
         const std::string rhsArgName = "__runtime_rhs__";
         Cell argsCell(Cell::List);
         argsCell.list.push_back(makeSymbolCell(lhsArgName));
         argsCell.list.push_back(makeSymbolCell(rhsArgName));

         Cell program(Cell::List);
         program.list.push_back(argsCell);
         if (closure->params[0] != lhsArgName)
            program.list.push_back(makeDefineCell(closure->params[0], makeSymbolCell(lhsArgName)));
         if (closure->params[1] != rhsArgName)
            program.list.push_back(makeDefineCell(closure->params[1], makeSymbolCell(rhsArgName)));

         std::set<std::string> seenBindings;
         std::vector<const gnine::Image *> inputImages;
         inputImages.push_back(&lhs);
         inputImages.push_back(&rhs);
         for (EnvironmentObject *current = closure->env; current != NULL; current = current->parent)
         {
            for (std::map<std::string, Value>::const_iterator it = current->bindings.begin();
                 it != current->bindings.end();
                 ++it)
            {
               if (!seenBindings.insert(it->first).second ||
                   it->first == closure->params[0] ||
                   it->first == closure->params[1])
                  continue;

               const Value &value = it->second;
               if (value.isNumber())
               {
                  program.list.push_back(makeDefineCell(it->first, doubleToCell(value.number)));
                  continue;
               }

               if (value.isExpr())
               {
                  program.list.push_back(makeDefineCell(it->first, value.expr));
                  continue;
               }

               if (value.isBuiltin())
                  continue;

               if (value.isObject() && value.object->type == Object::Image)
               {
                  argsCell.list.push_back(makeSymbolCell(it->first));
                  inputImages.push_back(static_cast<ImageObject *>(value.object)->image);
                  continue;
               }

               return false;
            }
         }

         program.list[0] = argsCell;
         program.list.push_back(closure->body);

         if (!ensureRuntimeJitInitialized())
            throw std::runtime_error("Failed to initialize JIT for compiled runtime zip-image");

         try
         {
            LoweredProgram lowered = lowerProgram(program);
            if (lowered.usesVectorFeatures || lowered.channelPrograms.size() != 1)
               return false;

            OMR::JitBuilder::TypeDictionary types;
            ImageArray method(&types);
            method.runByteCodes(lowered.channelPrograms[0], false);

            void *entry = NULL;
            int32_t rc = compileMethodBuilder(&method, &entry);
            if (rc != 0)
               return false;

            ImageArrayFunctionType *fn = reinterpret_cast<ImageArrayFunctionType *>(entry);
            gnine::Image resultImage(lhs.width(), lhs.height(), lhs.stride(), lhs.channelCount());
            std::vector<double *> dataPtrs;
            dataPtrs.reserve(inputImages.size());
            for (int channel = 0; channel < lhs.channelCount(); ++channel)
            {
               dataPtrs.clear();
               for (size_t idx = 0; idx < inputImages.size(); ++idx)
               {
                  const gnine::Image *image = inputImages[idx];
                  int sourceChannel = image->channelCount() == 1 ? 0 : channel;
                  dataPtrs.push_back(const_cast<double *>(image->getChannelData(sourceChannel)));
               }
               fn(lhs.width(), lhs.height(), 1, dataPtrs.data(), resultImage.getChannelData(channel));
            }

            *outResult = imageValue(resultImage);
            return true;
         }
         catch (...)
         {
            return false;
         }
      }

      Value Evaluator::applyMapImage(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (args.size() != 1)
            throw std::runtime_error(context + " expects exactly one image");

         const gnine::Image &source = *requireImageObject(args[0], context)->image;
         if (callable.isObject() && callable.object->type == Object::Closure)
         {
            Value compiledResult = Value::nil();
            if (tryCompiledMapImage(static_cast<ClosureObject *>(callable.object), source, &compiledResult))
               return compiledResult;
         }

         gnine::Image resultImage(source.width(), source.height(), source.stride(), source.channelCount());

         for (int channel = 0; channel < source.channelCount(); ++channel)
         {
            for (int row = 0; row < source.height(); ++row)
            {
               for (int col = 0; col < source.width(); ++col)
               {
                  PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                               row, col, channel);
                  std::vector<Value> pixelArgs(1, Value::numberValue(source(row, col, channel)));
                  Value pixelValue = applyCallable(callable, pixelArgs, context);
                  resultImage(row, col, channel) = requireNumber(pixelValue, context);
               }
            }
         }

         return imageValue(resultImage);
      }

      Value Evaluator::applyZipImage(const Value &callable,
                                     const std::vector<Value> &args,
                                     const std::string &context)
      {
         if (args.size() != 2)
            throw std::runtime_error(context + " expects exactly two images");

         const gnine::Image &lhs = *requireImageObject(args[0], context)->image;
         const gnine::Image &rhs = *requireImageObject(args[1], context)->image;
         if (lhs.width() != rhs.width() || lhs.height() != rhs.height() || lhs.channelCount() != rhs.channelCount())
            throw std::runtime_error(context + " requires matching image extents and channel counts");

         if (callable.isObject() && callable.object->type == Object::Closure)
         {
            Value compiledResult = Value::nil();
            if (tryCompiledZipImage(static_cast<ClosureObject *>(callable.object), lhs, rhs, &compiledResult))
               return compiledResult;
         }

         gnine::Image resultImage(lhs.width(), lhs.height(), lhs.stride(), lhs.channelCount());
         for (int channel = 0; channel < lhs.channelCount(); ++channel)
         {
            for (int row = 0; row < lhs.height(); ++row)
            {
               for (int col = 0; col < lhs.width(); ++col)
               {
                  PixelContextScope pixelScope(_hasPixelContext, _currentRow, _currentCol, _currentChannel,
                                               row, col, channel);
                  std::vector<Value> pixelArgs;
                  pixelArgs.push_back(Value::numberValue(lhs(row, col, channel)));
                  pixelArgs.push_back(Value::numberValue(rhs(row, col, channel)));
                  Value pixelValue = applyCallable(callable, pixelArgs, context);
                  resultImage(row, col, channel) = requireNumber(pixelValue, context);
               }
            }
         }

         return imageValue(resultImage);
      }

      Cell Evaluator::numberToCell(double value) const
      {
         std::ostringstream out;
         out << std::setprecision(17) << value;
         return Cell(Cell::Number, out.str());
      }

      std::vector<Cell> Evaluator::requireExprArgs(const std::vector<Value> &args,
                                                   const std::string &context) const
      {
         std::vector<Cell> exprArgs;
         exprArgs.reserve(args.size());
         for (size_t idx = 0; idx < args.size(); ++idx)
            exprArgs.push_back(requireExpr(args[idx], context));
         return exprArgs;
      }
   }
}
