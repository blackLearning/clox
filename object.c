#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"
#include "table.h"

#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);
  object->type = type;
  object->isMarked = false;

  // 每次分配一个对象的内存，将其放入链表的头部
  object->next = vm.objects;
  vm.objects = object;

  // debug模式下，每次分配新的对象之后都打印该对象的内存信息
  #ifdef DEBUG_LOG_GC
    printf("%p allocate %ld for %d\n", (void*)object, size, type);
  #endif

  return object;
}

ObjFunction* newFunction() {
  ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);

  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  initChunk(&function->chunk);

  return function;
}

ObjUpvalue* newUpvalue(Value* slot) {
  ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
  upvalue->location = slot;
  upvalue->next = NULL;
  upvalue->closed = NIL_VAL;
  return upvalue;
}

ObjClosure* newClosure(ObjFunction* function) {
  ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);

  ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) {                    
    upvalues[i] = NULL;                                                 
  }

  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  closure->function = function;

  return closure;
}

// 初始化一个内置函数
ObjNative* newNative(NativeFn function) {
  ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  native->function = function;

  return native;
}

ObjClass* newClass(ObjString* name) {
  ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
  klass->name = name;
  // 初始化一个HashTable用来存放methods
  initTable(&klass->methods);
  return klass;
}

ObjInstance* newInstance(ObjClass* klass) {
  ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
  instance->klass = klass;
  initTable(&instance->fields);
  return instance;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
  ObjBoundMethod* bound = ALLOCATE_OBJ(ObjBoundMethod,
                                       OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  return bound;
}

// 字符串hash方法
static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;

  for (int i = 0; i < length; i++) {
    hash ^= key[i];
    hash *= 16777619;
  }

  return hash;
}

// 分配一块内存空间以存储ObjString对象
static ObjString* allocateString(const char* chars, int length) {
  // 首先查看缓存
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) return interned;

  // 生成新的字符串对象
  size_t size = sizeof(ObjString) + sizeof(char) * (length + 1);
  ObjString* string = (ObjString*)allocateObject(size, OBJ_STRING);

  string->hash = hash;
  memcpy(string->chars, chars, length);
  string->chars[length] = '\0';
  string->length = length;

  // GC边界：同理tableSet扩容时可能会触发垃圾回收，因此在set之前需要将ObjString保持引用
  push(OBJ_VAL(string));

  // 每次生成一个字符串的时候，都将string对象收集到hashTable中
  tableSet(&vm.strings, string, NIL_VAL);

  pop();

  return string;
}

// 连接两个字符串对象
ObjString* concatenateString(ObjString* a, ObjString* b) {
  int length = a->length + b->length;
  size_t size = sizeof(ObjString) + sizeof(char) * (length + 1);
  ObjString* string = (ObjString*)allocateObject(size, OBJ_STRING);

  memcpy(string->chars, a->chars, a->length);
  memcpy(string->chars + a->length, b->chars, b->length);

  string->chars[length] = '\0';
  string->length = length;
  string->hash = hashString(string->chars, length);

  // 由于在lox中，字符串是不可变的，因此将所有的字符串对象保存在内存中，以便复用
  // 每当生成一个新的字符串对象时，检查table中是否有该字符串，如果有就复用
  ObjString* interned = tableFindString(&vm.strings, string->chars, length, string->hash);
  if (interned != NULL) {
    // 如果找到了缓存的字符串，将刚才创建的字符串对象回收
    freeObject((Obj*)string);
    return interned;
  }

  // GC边界：同理tableSet扩容时可能会触发垃圾回收，因此在set之前需要将ObjString保持引用
  push(OBJ_VAL(string));

  // 每次生成一个字符串的时候，都将string对象收集到hashTable中
  tableSet(&vm.strings, string, NIL_VAL);

  pop();

  return string;
}


// @Depreacted
ObjString* takeString(char* chars, int length) {
  return allocateString(chars, length);
}

// 分配一块内存空间以复制chars
ObjString* copyString(const char* chars, int length) {
  return allocateString(chars, length);
}

static void printFunction(ObjFunction* func) {
  // debug print
  if (func->name == NULL) {
    printf("<script>");
    return;
  }

  // 打印出函数名，一些更好的实现会打印出整个函数体
  printf("<fn %s>", func->name->chars);
}

void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
  case OBJ_STRING:
    printf("%s", AS_CSTRING(value));
    break;
  case OBJ_FUNCTION:
    printFunction(AS_FUNCTION(value));
    break;
  case OBJ_CLOSURE:
    printFunction(AS_CLOSURE(value)->function);
    break;
  case OBJ_UPVALUE:
    printf("upvalue");
    break;
  case OBJ_NATIVE:
    printf("<native fn>");
    break;
  case OBJ_CLASS:
    printf("%s", AS_CLASS(value)->name->chars);
    break;
  case OBJ_INSTANCE:
    printf("%s instance", AS_INSTANCE(value)->klass->name->chars);
    break;
  case OBJ_BOUND_METHOD:
    // 在用户的角度，绑定方法和普通函数是一样的
    printFunction(AS_BOUND_METHOD(value)->method->function);
    break;
  default:
    break;
  }
}
