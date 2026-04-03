#pragma once

#ifndef RUNTIME_INCL
#define RUNTIME_INCL

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "Parser.h"

namespace gnine
{
   namespace runtime
   {
      struct Object;
      struct EnvironmentObject;
      struct ClosureObject;

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
         std::string builtinName;
         Object *object;

         Value();

         static Value nil();
         static Value numberValue(double value);
         static Value exprValue(const Cell &expr);
         static Value builtinValue(const std::string &name);
         static Value objectValue(Object *value);

         bool isNil() const;
         bool isNumber() const;
         bool isExpr() const;
         bool isBuiltin() const;
         bool isObject() const;
      };

      struct Object
      {
         enum Type
         {
            Environment,
            Closure
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

         explicit EnvironmentObject(EnvironmentObject *parentEnv);
         void trace(class Heap &heap) override;
      };

      struct ClosureObject : public Object
      {
         std::vector<std::string> params;
         Cell body;
         EnvironmentObject *env;

         ClosureObject(const std::vector<std::string> &parameters,
                       const Cell &bodyExpr,
                       EnvironmentObject *capturedEnv);
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
                                        const Cell &body,
                                        EnvironmentObject *env);

         void addRoot(Value *slot);
         void removeRoot(Value *slot);

         void collect();
         void markValue(const Value &value);
         void markObject(Object *object);

         size_t liveObjectCount() const;
         size_t collectionCount() const;

      private:
         template <typename T, typename... Args>
         T *allocateObject(Args... args);

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
         Value evaluateExpr(const Cell &expr);
         Cell normalizeProgram(const Cell &program);
         Cell requireExpr(const Value &value, const std::string &context) const;

         Heap &heap();

      private:
         Value eval(const Cell &expr, EnvironmentObject *env);
         Value applyBuiltin(const std::string &builtinName,
                            const std::vector<Value> &args);
         Value applyCallable(const Value &callable,
                             const std::vector<Value> &args,
                             const std::string &context);
         Value applyClosure(ClosureObject *closure,
                            const std::vector<Value> &args);
         bool lookup(EnvironmentObject *env,
                     const std::string &name,
                     Value *outValue) const;
         bool isBuiltin(const std::string &name) const;
         bool allNumbers(const std::vector<Value> &args) const;
         double requireNumber(const Value &value,
                              const std::string &context) const;
         Cell numberToCell(double value) const;
         std::vector<Cell> requireExprArgs(const std::vector<Value> &args,
                                           const std::string &context) const;

         Heap _heap;
      };
   }
}

#endif // RUNTIME_INCL
