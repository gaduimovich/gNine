#pragma once

#ifndef GNINE_RUNTIME_INCL
#define GNINE_RUNTIME_INCL

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Image.h"
#include "Parser.h"

namespace gnine
{
   namespace runtime
   {
      struct Object;
      struct EnvironmentObject;
      struct ClosureObject;
      struct ImageObject;
      struct TupleObject;

      struct Value
      {
         enum Kind
         {
            Nil,
            Number,
            Expr,
            Builtin,
            ObjectRef
         };

         Kind kind;
         double number;
         Cell expr;
         bool hasSourceExpr;
         Cell sourceExpr;
         std::string builtinName;
         Object *object;

         Value();

         static Value nil();
         static Value numberValue(double value);
         static Value numberValue(double value, const Cell &sourceExpr);
         static Value exprValue(const Cell &expr);
         static Value builtinValue(const std::string &name);
         static Value objectValue(Object *value);

         bool isNil() const;
         bool isNumber() const;
         bool isExpr() const;
         bool isBuiltin() const;
         bool isObject() const;
         bool hasNumberSourceExpr() const;
      };

         struct Object
         {
            enum Type
            {
               Environment,
               Closure,
               Image,
               Tuple
            };

         Type type;
         bool marked;
         Object *next;

         explicit Object(Type objectType);
         virtual ~Object();
         virtual void trace(class Heap &heap) = 0;
      };

      struct EnvironmentObject : public Object
      {
         EnvironmentObject *parent;
         std::map<std::string, Value> bindings;
         std::map<std::string, Cell> sourceExprs;

         explicit EnvironmentObject(EnvironmentObject *parentEnv);
         void trace(class Heap &heap) override;
      };

      struct ClosureObject : public Object
      {
         std::vector<Cell> paramPatterns;
         std::vector<std::string> params;
         Cell body;
         EnvironmentObject *env;

         ClosureObject(const std::vector<Cell> &patterns,
                       const std::vector<std::string> &parameters,
                       const Cell &bodyExpr,
                       EnvironmentObject *capturedEnv);
         void trace(class Heap &heap) override;
      };

      struct ImageObject : public Object
      {
         gnine::Image *image;

         explicit ImageObject(const gnine::Image &source);
         ~ImageObject() override;
         void trace(class Heap &heap) override;
      };

      struct TupleObject : public Object
      {
         std::vector<Value> values;

         explicit TupleObject(const std::vector<Value> &elements);
         void trace(class Heap &heap) override;
      };

      class Heap
      {
      public:
         class Root
         {
         public:
            Root(Heap &heap, Value &slot);
            ~Root();

         private:
            Heap *_heap;
            Value *_slot;

            Root(const Root &);
            Root &operator=(const Root &);
         };

         Heap();
         ~Heap();

         EnvironmentObject *allocateEnvironment(EnvironmentObject *parent);
         ClosureObject *allocateClosure(const std::vector<std::string> &params,
                                        const std::vector<Cell> &patterns,
                                        const Cell &body,
                                        EnvironmentObject *env);
         ImageObject *allocateImage(const gnine::Image &image);
         TupleObject *allocateTuple(const std::vector<Value> &values);

         void addRoot(Value *slot);
         void removeRoot(Value *slot);

         void collect();
         void markValue(const Value &value);
         void markObject(Object *object);

         size_t liveObjectCount() const;
         size_t collectionCount() const;

      private:
         template <typename T, typename... Args>
         T *allocateObject(Args&&... args);

         void sweep();

         Object *_objects;
         size_t _liveObjects;
         size_t _collectionCount;
         size_t _nextCollectionThreshold;
         std::vector<Value *> _roots;

         Heap(const Heap &);
         Heap &operator=(const Heap &);
      };

      class Evaluator
      {
      public:
         Evaluator();

         Value evaluateProgram(const Cell &program);
         Value evaluateProgram(const Cell &program, const std::map<std::string, Value> &bindings);
         Value evaluateProgram(const Cell &program,
                               const std::map<std::string, Value> &bindings,
                               std::map<std::string, Value> *outBindings);
         Value evaluateExpr(const Cell &expr);
         Value evaluateExpr(const Cell &expr, const std::map<std::string, Value> &bindings);
         Cell normalizeProgram(const Cell &program);
         Cell requireExpr(const Value &value, const std::string &context) const;
         Value imageValue(const gnine::Image &image);
         void clearExecutionTrace();
         const std::vector<std::string> &executionTrace() const;

         Heap &heap();

      private:
         struct ProgramStatement
         {
            const Cell *expr;
            std::string defineName;
            bool isDefine;
         };

         struct ProgramMetadata
         {
            std::vector<std::string> argBindingNames;
            std::vector<ProgramStatement> statements;
         };

         struct CompiledCanvasChannelMetadata
         {
            Cell specializedBody;
            std::vector<std::string> referencedSymbols;
         };

         const ProgramMetadata &programMetadata(const Cell &program);
         const CompiledCanvasChannelMetadata &compiledCanvasChannelMetadata(const Cell &body,
                                                                           int channel);
         Value eval(const Cell &expr, EnvironmentObject *env);
         Value applyBuiltin(const std::string &builtinName,
                            const std::vector<Value> &args);
         Value applyCallable(const Value &callable,
                             const std::vector<Value> &args,
                             const std::string &context);
         Value applyImageSample(ImageObject *image,
                                const std::vector<Value> &args,
                                const std::string &context);
         Value applyAbsoluteImageSample(const std::vector<Value> &args,
                                        const std::string &context);
         Value applyClosure(ClosureObject *closure,
                            const std::vector<Value> &args);
         void bindPattern(EnvironmentObject *env,
                          const Cell &pattern,
                          const Value &value);
         void bindPattern(EnvironmentObject *env,
                          const Cell &pattern,
                          const Value &value,
                          const Value &symbolicValue,
                          EnvironmentObject *symbolicEnv);
         bool lookup(EnvironmentObject *env,
                     const std::string &name,
                     Value *outValue) const;
         bool isBuiltin(const std::string &name) const;
         bool allNumbers(const std::vector<Value> &args) const;
         bool allConcreteBuiltinArgs(const std::vector<Value> &args) const;
         double requireNumber(const Value &value,
                              const std::string &context) const;
         ImageObject *requireImageObject(const Value &value,
                                         const std::string &context) const;
         TupleObject *requireTupleObject(const Value &value,
                                         const std::string &context) const;
         bool tryCompiledMapImage(ClosureObject *closure,
                                  const gnine::Image &source,
                                  Value *outResult,
                                  std::string *fallbackReason);
         bool tryCompiledZipImage(ClosureObject *closure,
                                  const gnine::Image &lhs,
                                  const gnine::Image &rhs,
                                  Value *outResult,
                                  std::string *fallbackReason);
         Value applyMapImage(const Value &callable,
                             const std::vector<Value> &args,
                             const std::string &context);
         Value applyZipImage(const Value &callable,
                             const std::vector<Value> &args,
                             const std::string &context);
         bool tryCompiledCanvas(int width,
                                int height,
                                int channels,
                                const Cell &body,
                                EnvironmentObject *env,
                                Value *outResult,
                                std::string *fallbackReason);
         Value applyCanvas(int width,
                           int height,
                           int channels,
                           const Cell &body,
                           EnvironmentObject *env,
                           const std::string &context);
         void traceExecution(const std::string &entry);
         void traceEnvironmentBindings(EnvironmentObject *env,
                                       const std::string &tracePrefix,
                                       int maxClosureDepth);
         void traceClosureBindings(ClosureObject *closure,
                                   const std::string &tracePrefix,
                                   int maxClosureDepth);
         Cell numberToCell(double value) const;
         std::vector<Cell> requireExprArgs(const std::vector<Value> &args,
                                           const std::string &context) const;

         Heap _heap;
         bool _hasPixelContext;
         int _currentRow;
         int _currentCol;
         int _currentChannel;
         bool _reportedNonNumericPixel;
         int _compiledScalarScratchWidth;
         int _compiledScalarScratchHeight;
         std::vector<gnine::Image> _compiledScalarScratch;
         std::map<const Cell *, ProgramMetadata> _programMetadataCache;
         std::map<std::pair<const Cell *, int>, CompiledCanvasChannelMetadata> _compiledCanvasChannelMetadataCache;
         std::vector<std::string> _executionTrace;
      };
   }
}

#endif // GNINE_RUNTIME_INCL
