// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/dart_api_state.h"
#include "vm/message.h"
#include "vm/native_entry.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/regexp.h"
#include "vm/snapshot.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"
#include "vm/type_testing_stubs.h"
#include "vm/visitor.h"

namespace dart {

// TODO(dartbug.com/34796): enable or remove this optimization.
DEFINE_FLAG(
    uint64_t,
    externalize_typed_data_threshold,
    kMaxUint64,
    "Convert TypedData to ExternalTypedData when sending through a message"
    " port after it exceeds certain size in bytes.");

#define OFFSET_OF_FROM(obj)                                                    \
  obj.ptr()->from() - reinterpret_cast<ObjectPtr*>(obj.ptr()->untag())

#define READ_OBJECT_FIELDS(object, from, to, as_reference)                     \
  intptr_t num_flds = (to) - (from);                                           \
  for (intptr_t i = 0; i <= num_flds; i++) {                                   \
    (*reader->PassiveObjectHandle()) = reader->ReadObjectImpl(as_reference);   \
    object.StorePointer(((from) + i), reader->PassiveObjectHandle()->ptr());   \
  }

#define READ_COMPRESSED_OBJECT_FIELDS(object, from, to, as_reference)          \
  intptr_t num_flds = (to) - (from);                                           \
  for (intptr_t i = 0; i <= num_flds; i++) {                                   \
    (*reader->PassiveObjectHandle()) = reader->ReadObjectImpl(as_reference);   \
    object.StoreCompressedPointer(((from) + i),                                \
                                  reader->PassiveObjectHandle()->ptr());       \
  }

ClassPtr Class::ReadFrom(SnapshotReader* reader,
                         intptr_t object_id,
                         intptr_t tags,
                         Snapshot::Kind kind,
                         bool as_reference) {
  ASSERT(reader != NULL);

  Class& cls = Class::ZoneHandle(reader->zone(), Class::null());
  cls = reader->ReadClassId(object_id);
  return cls.ptr();
}

void UntaggedClass::WriteTo(SnapshotWriter* writer,
                            intptr_t object_id,
                            Snapshot::Kind kind,
                            bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kClassCid);
  writer->WriteTags(writer->GetObjectTags(this));

  if (writer->can_send_any_object() ||
      writer->AllowObjectsInDartLibrary(library())) {
    writer->WriteClassId(this);
  } else {
    // We do not allow regular dart instances in isolate messages.
    writer->SetWriteException(Exceptions::kArgument,
                              "Illegal argument in isolate message"
                              " : (object is a regular Dart Instance)");
  }
}

TypePtr Type::ReadFrom(SnapshotReader* reader,
                       intptr_t object_id,
                       intptr_t tags,
                       Snapshot::Kind kind,
                       bool as_reference) {
  ASSERT(reader != NULL);

  // Determine if the type class of this type is in the full snapshot.
  reader->Read<bool>();

  // Allocate type object.
  Type& type = Type::ZoneHandle(reader->zone(), Type::New());
  bool is_canonical = UntaggedObject::IsCanonical(tags);
  reader->AddBackRef(object_id, &type, kIsDeserialized);

  // Set all non object fields.
  const uint8_t combined = reader->Read<uint8_t>();
  type.set_type_state(combined >> 4);
  type.set_nullability(static_cast<Nullability>(combined & 0xf));

  // Read the code object for the type testing stub and set its entrypoint.
  reader->EnqueueTypePostprocessing(type);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(type, type.ptr()->untag()->from(),
                                type.ptr()->untag()->to(), as_reference);

  // Read in the type class.
  (*reader->ClassHandle()) =
      Class::RawCast(reader->ReadObjectImpl(as_reference));
  type.set_type_class(*reader->ClassHandle());

  // Fill in the type testing stub.
  Code& code = *reader->CodeHandle();
  code = TypeTestingStubGenerator::DefaultCodeForType(type);
  type.InitializeTypeTestingStubNonAtomic(code);

  if (is_canonical) {
    type ^= type.Canonicalize(Thread::Current(), nullptr);
  }

  return type.ptr();
}

void UntaggedType::WriteTo(SnapshotWriter* writer,
                           intptr_t object_id,
                           Snapshot::Kind kind,
                           bool as_reference) {
  ASSERT(writer != NULL);

  // Only resolved and finalized types should be written to a snapshot.
  ASSERT((type_state_ == UntaggedType::kFinalizedInstantiated) ||
         (type_state_ == UntaggedType::kFinalizedUninstantiated));
  ASSERT(type_class_id() != Object::null());

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kTypeCid);
  writer->WriteTags(writer->GetObjectTags(this));

  if (type_class_id()->IsHeapObject()) {
    // Type class is still an unresolved class.
    UNREACHABLE();
  }

  // Lookup the type class.
  SmiPtr raw_type_class_id = Smi::RawCast(type_class_id());
  ClassPtr type_class =
      writer->isolate_group()->class_table()->At(Smi::Value(raw_type_class_id));

  // Write out typeclass_is_in_fullsnapshot first as this will
  // help the reader decide on how to canonicalize the type object.
  intptr_t tags = writer->GetObjectTags(type_class);
  bool typeclass_is_in_fullsnapshot =
      (ClassIdTag::decode(tags) == kClassCid) &&
      Class::IsInFullSnapshot(static_cast<ClassPtr>(type_class));
  writer->Write<bool>(typeclass_is_in_fullsnapshot);

  // Write out all the non object pointer fields.
  const uint8_t combined = (type_state_ << 4) | nullability_;
  ASSERT(type_state_ == (combined >> 4));
  ASSERT(nullability_ == (combined & 0xf));
  writer->Write<uint8_t>(combined);

  // Write out all the object pointer fields.
  ASSERT(type_class_id() != Object::null());
  SnapshotWriterVisitor visitor(writer, as_reference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());

  // Write out the type class.
  writer->WriteObjectImpl(type_class, as_reference);
}

TypeRefPtr TypeRef::ReadFrom(SnapshotReader* reader,
                             intptr_t object_id,
                             intptr_t tags,
                             Snapshot::Kind kind,
                             bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate type ref object.
  TypeRef& type_ref = TypeRef::ZoneHandle(reader->zone(), TypeRef::New());
  reader->AddBackRef(object_id, &type_ref, kIsDeserialized);

  // Read the code object for the type testing stub and set its entrypoint.
  reader->EnqueueTypePostprocessing(type_ref);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(type_ref, type_ref.ptr()->untag()->from(),
                                type_ref.ptr()->untag()->to(), kAsReference);

  // Fill in the type testing stub.
  Code& code = *reader->CodeHandle();
  code = TypeTestingStubGenerator::DefaultCodeForType(type_ref);
  type_ref.InitializeTypeTestingStubNonAtomic(code);

  return type_ref.ptr();
}

void UntaggedTypeRef::WriteTo(SnapshotWriter* writer,
                              intptr_t object_id,
                              Snapshot::Kind kind,
                              bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kTypeRefCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

TypeParameterPtr TypeParameter::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate type parameter object.
  TypeParameter& type_parameter =
      TypeParameter::ZoneHandle(reader->zone(), TypeParameter::New());
  bool is_canonical = UntaggedObject::IsCanonical(tags);
  reader->AddBackRef(object_id, &type_parameter, kIsDeserialized);

  // Set all non object fields.
  type_parameter.set_base(reader->Read<uint8_t>());
  type_parameter.set_index(reader->Read<uint8_t>());
  const uint8_t combined = reader->Read<uint8_t>();
  type_parameter.set_flags(combined >> 4);
  type_parameter.set_nullability(static_cast<Nullability>(combined & 0xf));

  // Read the code object for the type testing stub and set its entrypoint.
  reader->EnqueueTypePostprocessing(type_parameter);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(
      type_parameter, type_parameter.ptr()->untag()->from(),
      type_parameter.ptr()->untag()->to(), kAsReference);

  // Read in the parameterized class.
  (*reader->ClassHandle()) =
      Class::RawCast(reader->ReadObjectImpl(kAsReference));
  if (reader->ClassHandle()->id() == kFunctionCid) {
    (*reader->ClassHandle()) = Class::null();
  }
  type_parameter.set_parameterized_class(*reader->ClassHandle());

  // Fill in the type testing stub.
  Code& code = *reader->CodeHandle();
  code = TypeTestingStubGenerator::DefaultCodeForType(type_parameter);
  type_parameter.InitializeTypeTestingStubNonAtomic(code);

  if (is_canonical) {
    type_parameter ^= type_parameter.Canonicalize(Thread::Current(), nullptr);
  }

  return type_parameter.ptr();
}

void UntaggedTypeParameter::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  ASSERT(writer != NULL);

  // Only finalized type parameters should be written to a snapshot.
  ASSERT(FinalizedBit::decode(flags_));

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kTypeParameterCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the non object pointer fields.
  writer->Write<uint8_t>(base_);
  writer->Write<uint8_t>(index_);
  const uint8_t combined = (flags_ << 4) | nullability_;
  ASSERT(flags_ == (combined >> 4));
  ASSERT(nullability_ == (combined & 0xf));
  writer->Write<uint8_t>(combined);

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());

  // Write out the parameterized class (or Function if cid == kFunctionCid).
  ClassPtr param_class =
      writer->isolate_group()->class_table()->At(parameterized_class_id_);
  writer->WriteObjectImpl(param_class, kAsReference);
}

TypeParametersPtr TypeParameters::ReadFrom(SnapshotReader* reader,
                                           intptr_t object_id,
                                           intptr_t tags,
                                           Snapshot::Kind kind,
                                           bool as_reference) {
  ASSERT(reader != NULL);

  TypeParameters& type_parameters =
      TypeParameters::ZoneHandle(reader->zone(), TypeParameters::New());
  reader->AddBackRef(object_id, &type_parameters, kIsDeserialized);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(
      type_parameters, type_parameters.ptr()->untag()->from(),
      type_parameters.ptr()->untag()->to(), kAsReference);

  return type_parameters.ptr();
}

void UntaggedTypeParameters::WriteTo(SnapshotWriter* writer,
                                     intptr_t object_id,
                                     Snapshot::Kind kind,
                                     bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kTypeParametersCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

TypeArgumentsPtr TypeArguments::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  ASSERT(reader != NULL);

  // Read the length so that we can determine instance size to allocate.
  intptr_t len = reader->ReadSmiValue();

  TypeArguments& type_arguments =
      TypeArguments::ZoneHandle(reader->zone(), TypeArguments::New(len));
  bool is_canonical = UntaggedObject::IsCanonical(tags);
  reader->AddBackRef(object_id, &type_arguments, kIsDeserialized);

  // Set the instantiations field, which is only read from a full snapshot.
  type_arguments.set_instantiations(Object::zero_array());

  // Now set all the type fields.
  for (intptr_t i = 0; i < len; i++) {
    *reader->TypeHandle() ^= reader->ReadObjectImpl(as_reference);
    type_arguments.SetTypeAt(i, *reader->TypeHandle());
  }

  // Set the canonical bit.
  if (is_canonical) {
    type_arguments = type_arguments.Canonicalize(Thread::Current(), nullptr);
  }

  return type_arguments.ptr();
}

void UntaggedTypeArguments::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kTypeArgumentsCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the length field.
  writer->Write<ObjectPtr>(length());

  // Write out the individual types.
  intptr_t len = Smi::Value(length());
  for (intptr_t i = 0; i < len; i++) {
    // The Dart VM reuses type argument lists across instances in order
    // to reduce memory footprint, this can sometimes lead to a type from
    // such a shared type argument list being sent over to another isolate.
    // In such scenarios where it is not appropriate to send the types
    // across (isolates spawned using spawnURI) we send them as dynamic.
    if (!writer->can_send_any_object()) {
      // Lookup the type class.
      TypePtr raw_type = Type::RawCast(element(i));
      SmiPtr raw_type_class_id =
          Smi::RawCast(raw_type->untag()->type_class_id());
      ClassPtr type_class = writer->isolate_group()->class_table()->At(
          Smi::Value(raw_type_class_id));
      if (!writer->AllowObjectsInDartLibrary(type_class->untag()->library())) {
        writer->WriteVMIsolateObject(kDynamicType);
      } else {
        writer->WriteObjectImpl(element(i), as_reference);
      }
    } else {
      writer->WriteObjectImpl(element(i), as_reference);
    }
  }
}

ClosurePtr Closure::ReadFrom(SnapshotReader* reader,
                             intptr_t object_id,
                             intptr_t tags,
                             Snapshot::Kind kind,
                             bool as_reference) {
  UNREACHABLE();
  return Closure::null();
}

void UntaggedClosure::WriteTo(SnapshotWriter* writer,
                              intptr_t object_id,
                              Snapshot::Kind kind,
                              bool as_reference) {
  ASSERT(writer != NULL);
  ASSERT(kind == Snapshot::kMessage);

  // Check if closure is serializable, throw an exception otherwise.
  FunctionPtr func = writer->IsSerializableClosure(ClosurePtr(this));
  if (func != Function::null()) {
    writer->WriteStaticImplicitClosure(
        object_id, func, writer->GetObjectTags(this), delayed_type_arguments());
    return;
  }

  UNREACHABLE();
}

ContextPtr Context::ReadFrom(SnapshotReader* reader,
                             intptr_t object_id,
                             intptr_t tags,
                             Snapshot::Kind kind,
                             bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate context object.
  int32_t num_vars = reader->Read<int32_t>();
  Context& context = Context::ZoneHandle(reader->zone());
  reader->AddBackRef(object_id, &context, kIsDeserialized);
  if (num_vars != 0) {
    context = Context::New(num_vars);

    // Set all the object fields.
    // TODO(5411462): Need to assert No GC can happen here, even though
    // allocations may happen.
    intptr_t num_flds =
        (context.ptr()->untag()->to(num_vars) - context.ptr()->untag()->from());
    for (intptr_t i = 0; i <= num_flds; i++) {
      (*reader->PassiveObjectHandle()) = reader->ReadObjectImpl(kAsReference);
      context.StorePointer((context.ptr()->untag()->from() + i),
                           reader->PassiveObjectHandle()->ptr());
    }
  }
  return context.ptr();
}

void UntaggedContext::WriteTo(SnapshotWriter* writer,
                              intptr_t object_id,
                              Snapshot::Kind kind,
                              bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kContextCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out num of variables in the context.
  const int32_t num_variables = num_variables_;
  writer->Write<int32_t>(num_variables);
  if (num_variables != 0) {
    // Write out all the object pointer fields.
    SnapshotWriterVisitor visitor(writer, kAsReference);
    visitor.VisitPointers(from(), to(num_variables));
  }
}

ContextScopePtr ContextScope::ReadFrom(SnapshotReader* reader,
                                       intptr_t object_id,
                                       intptr_t tags,
                                       Snapshot::Kind kind,
                                       bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate context object.
  bool is_implicit = reader->Read<bool>();
  if (is_implicit) {
    ContextScope& context_scope = ContextScope::ZoneHandle(reader->zone());
    context_scope = ContextScope::New(1, true);
    reader->AddBackRef(object_id, &context_scope, kIsDeserialized);

    *reader->TypeHandle() ^= reader->ReadObjectImpl(kAsInlinedObject);

    // Create a descriptor for 'this' variable.
    context_scope.SetTokenIndexAt(0, TokenPosition::kMinSource);
    context_scope.SetDeclarationTokenIndexAt(0, TokenPosition::kMinSource);
    context_scope.SetNameAt(0, Symbols::This());
    context_scope.SetIsFinalAt(0, true);
    context_scope.SetIsConstAt(0, false);
    context_scope.SetTypeAt(0, *reader->TypeHandle());
    context_scope.SetContextIndexAt(0, 0);
    context_scope.SetContextLevelAt(0, 0);
    return context_scope.ptr();
  }
  UNREACHABLE();
  return NULL;
}

void UntaggedContextScope::WriteTo(SnapshotWriter* writer,
                                   intptr_t object_id,
                                   Snapshot::Kind kind,
                                   bool as_reference) {
  ASSERT(writer != NULL);

  if (is_implicit_) {
    ASSERT(num_variables_ == 1);
    const VariableDesc* var = VariableDescAddr(0);

    // Write out the serialization header value for this object.
    writer->WriteInlinedObjectHeader(object_id);

    // Write out the class and tags information.
    writer->WriteVMIsolateObject(kContextScopeCid);
    writer->WriteTags(writer->GetObjectTags(this));

    // Write out is_implicit flag for the context scope.
    writer->Write<bool>(true);

    // Write out the type of 'this' the variable.
    writer->WriteObjectImpl(var->type.Decompress(heap_base()),
                            kAsInlinedObject);

    return;
  }
  UNREACHABLE();
}

#define MESSAGE_SNAPSHOT_UNREACHABLE(type)                                     \
  type##Ptr type::ReadFrom(SnapshotReader* reader, intptr_t object_id,         \
                           intptr_t tags, Snapshot::Kind kind,                 \
                           bool as_reference) {                                \
    UNREACHABLE();                                                             \
    return type::null();                                                       \
  }                                                                            \
  void Untagged##type::WriteTo(SnapshotWriter* writer, intptr_t object_id,     \
                               Snapshot::Kind kind, bool as_reference) {       \
    UNREACHABLE();                                                             \
  }

#define MESSAGE_SNAPSHOT_ILLEGAL(type)                                         \
  type##Ptr type::ReadFrom(SnapshotReader* reader, intptr_t object_id,         \
                           intptr_t tags, Snapshot::Kind kind,                 \
                           bool as_reference) {                                \
    UNREACHABLE();                                                             \
    return type::null();                                                       \
  }                                                                            \
  void Untagged##type::WriteTo(SnapshotWriter* writer, intptr_t object_id,     \
                               Snapshot::Kind kind, bool as_reference) {       \
    writer->SetWriteException(Exceptions::kArgument,                           \
                              "Illegal argument in isolate message"            \
                              " : (object is a " #type ")");                   \
  }

MESSAGE_SNAPSHOT_UNREACHABLE(AbstractType);
MESSAGE_SNAPSHOT_UNREACHABLE(Bool);
MESSAGE_SNAPSHOT_UNREACHABLE(ClosureData);
MESSAGE_SNAPSHOT_UNREACHABLE(Code);
MESSAGE_SNAPSHOT_UNREACHABLE(CodeSourceMap);
MESSAGE_SNAPSHOT_UNREACHABLE(CompressedStackMaps);
MESSAGE_SNAPSHOT_UNREACHABLE(Error);
MESSAGE_SNAPSHOT_UNREACHABLE(ExceptionHandlers);
MESSAGE_SNAPSHOT_UNREACHABLE(FfiTrampolineData);
MESSAGE_SNAPSHOT_UNREACHABLE(Field);
MESSAGE_SNAPSHOT_UNREACHABLE(Function);
MESSAGE_SNAPSHOT_UNREACHABLE(CallSiteData);
MESSAGE_SNAPSHOT_UNREACHABLE(ICData);
MESSAGE_SNAPSHOT_UNREACHABLE(Instructions);
MESSAGE_SNAPSHOT_UNREACHABLE(InstructionsSection);
MESSAGE_SNAPSHOT_UNREACHABLE(InstructionsTable);
MESSAGE_SNAPSHOT_UNREACHABLE(KernelProgramInfo);
MESSAGE_SNAPSHOT_UNREACHABLE(Library);
MESSAGE_SNAPSHOT_UNREACHABLE(LibraryPrefix);
MESSAGE_SNAPSHOT_UNREACHABLE(LocalVarDescriptors);
MESSAGE_SNAPSHOT_UNREACHABLE(MegamorphicCache);
MESSAGE_SNAPSHOT_UNREACHABLE(Namespace);
MESSAGE_SNAPSHOT_UNREACHABLE(ObjectPool);
MESSAGE_SNAPSHOT_UNREACHABLE(PatchClass);
MESSAGE_SNAPSHOT_UNREACHABLE(PcDescriptors);
MESSAGE_SNAPSHOT_UNREACHABLE(Script);
MESSAGE_SNAPSHOT_UNREACHABLE(Sentinel);
MESSAGE_SNAPSHOT_UNREACHABLE(SingleTargetCache);
MESSAGE_SNAPSHOT_UNREACHABLE(String);
MESSAGE_SNAPSHOT_UNREACHABLE(SubtypeTestCache);
MESSAGE_SNAPSHOT_UNREACHABLE(LoadingUnit);
MESSAGE_SNAPSHOT_UNREACHABLE(TypedDataBase);
MESSAGE_SNAPSHOT_UNREACHABLE(UnlinkedCall);
MESSAGE_SNAPSHOT_UNREACHABLE(MonomorphicSmiableCall);
MESSAGE_SNAPSHOT_UNREACHABLE(UnwindError);
MESSAGE_SNAPSHOT_UNREACHABLE(FutureOr);
MESSAGE_SNAPSHOT_UNREACHABLE(WeakSerializationReference);

MESSAGE_SNAPSHOT_ILLEGAL(FunctionType)
MESSAGE_SNAPSHOT_ILLEGAL(DynamicLibrary);
MESSAGE_SNAPSHOT_ILLEGAL(MirrorReference);
MESSAGE_SNAPSHOT_ILLEGAL(Pointer);
MESSAGE_SNAPSHOT_ILLEGAL(ReceivePort);
MESSAGE_SNAPSHOT_ILLEGAL(StackTrace);
MESSAGE_SNAPSHOT_ILLEGAL(UserTag);

ApiErrorPtr ApiError::ReadFrom(SnapshotReader* reader,
                               intptr_t object_id,
                               intptr_t tags,
                               Snapshot::Kind kind,
                               bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate ApiError object.
  ApiError& api_error = ApiError::ZoneHandle(reader->zone(), ApiError::New());
  reader->AddBackRef(object_id, &api_error, kIsDeserialized);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(api_error, api_error.ptr()->untag()->from(),
                                api_error.ptr()->untag()->to(), kAsReference);

  return api_error.ptr();
}

void UntaggedApiError::WriteTo(SnapshotWriter* writer,
                               intptr_t object_id,
                               Snapshot::Kind kind,
                               bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kApiErrorCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

LanguageErrorPtr LanguageError::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate LanguageError object.
  LanguageError& language_error =
      LanguageError::ZoneHandle(reader->zone(), LanguageError::New());
  reader->AddBackRef(object_id, &language_error, kIsDeserialized);

  // Set all non object fields.
  language_error.set_token_pos(
      TokenPosition::Deserialize(reader->Read<int32_t>()));
  language_error.set_report_after_token(reader->Read<bool>());
  language_error.set_kind(reader->Read<uint8_t>());

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(
      language_error, language_error.ptr()->untag()->from(),
      language_error.ptr()->untag()->to(), kAsReference);

  return language_error.ptr();
}

void UntaggedLanguageError::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kLanguageErrorCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the non object fields.
  writer->Write<int32_t>(token_pos_.Serialize());
  writer->Write<bool>(report_after_token_);
  writer->Write<uint8_t>(kind_);

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

UnhandledExceptionPtr UnhandledException::ReadFrom(SnapshotReader* reader,
                                                   intptr_t object_id,
                                                   intptr_t tags,
                                                   Snapshot::Kind kind,
                                                   bool as_reference) {
  UnhandledException& result =
      UnhandledException::ZoneHandle(reader->zone(), UnhandledException::New());
  reader->AddBackRef(object_id, &result, kIsDeserialized);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(result, result.ptr()->untag()->from(),
                                result.ptr()->untag()->to(), kAsReference);

  return result.ptr();
}

void UntaggedUnhandledException::WriteTo(SnapshotWriter* writer,
                                         intptr_t object_id,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteVMIsolateObject(kUnhandledExceptionCid);
  writer->WriteTags(writer->GetObjectTags(this));
  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

InstancePtr Instance::ReadFrom(SnapshotReader* reader,
                               intptr_t object_id,
                               intptr_t tags,
                               Snapshot::Kind kind,
                               bool as_reference) {
  ASSERT(reader != NULL);

  // Create an Instance object or get canonical one if it is a canonical
  // constant.
  Instance& obj = Instance::ZoneHandle(reader->zone(), Instance::null());
  obj ^= Object::Allocate(kInstanceCid, Instance::InstanceSize(), Heap::kNew,
                          Instance::ContainsCompressedPointers());
  if (UntaggedObject::IsCanonical(tags)) {
    obj = obj.Canonicalize(reader->thread());
  }
  reader->AddBackRef(object_id, &obj, kIsDeserialized);

  return obj.ptr();
}

void UntaggedInstance::WriteTo(SnapshotWriter* writer,
                               intptr_t object_id,
                               Snapshot::Kind kind,
                               bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kInstanceCid);
  writer->WriteTags(writer->GetObjectTags(this));
}

IntegerPtr Mint::ReadFrom(SnapshotReader* reader,
                          intptr_t object_id,
                          intptr_t tags,
                          Snapshot::Kind kind,
                          bool as_reference) {
  ASSERT(reader != NULL);

  // Read the 64 bit value for the object.
  int64_t value = reader->Read<int64_t>();

  // Check if the value could potentially fit in a Smi in our current
  // architecture, if so return the object as a Smi.
  if (Smi::IsValid(value)) {
    Smi& smi =
        Smi::ZoneHandle(reader->zone(), Smi::New(static_cast<intptr_t>(value)));
    reader->AddBackRef(object_id, &smi, kIsDeserialized);
    return smi.ptr();
  }

  // Create a Mint object or get canonical one if it is a canonical constant.
  Mint& mint = Mint::ZoneHandle(reader->zone(), Mint::null());
  // When reading a script snapshot we need to canonicalize only those object
  // references that are objects from the core library (loaded from a
  // full snapshot). Objects that are only in the script need not be
  // canonicalized as they are already canonical.
  // When reading a message snapshot we always have to canonicalize.
  if (UntaggedObject::IsCanonical(tags)) {
    mint = Mint::NewCanonical(value);
    ASSERT(mint.IsCanonical());
  } else {
    mint = Mint::New(value);
  }
  reader->AddBackRef(object_id, &mint, kIsDeserialized);
  return mint.ptr();
}

void UntaggedMint::WriteTo(SnapshotWriter* writer,
                           intptr_t object_id,
                           Snapshot::Kind kind,
                           bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kMintCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the 64 bit value.
  writer->Write<int64_t>(value_);
}

DoublePtr Double::ReadFrom(SnapshotReader* reader,
                           intptr_t object_id,
                           intptr_t tags,
                           Snapshot::Kind kind,
                           bool as_reference) {
  ASSERT(reader != NULL);
  ASSERT(kind != Snapshot::kMessage);
  // Read the double value for the object.
  double value = reader->ReadDouble();

  // Create a Double object or get canonical one if it is a canonical constant.
  Double& dbl = Double::ZoneHandle(reader->zone(), Double::null());
  // When reading a script snapshot we need to canonicalize only those object
  // references that are objects from the core library (loaded from a
  // full snapshot). Objects that are only in the script need not be
  // canonicalized as they are already canonical.
  if (UntaggedObject::IsCanonical(tags)) {
    dbl = Double::NewCanonical(value);
    ASSERT(dbl.IsCanonical());
  } else {
    dbl = Double::New(value);
  }
  reader->AddBackRef(object_id, &dbl, kIsDeserialized);
  return dbl.ptr();
}

void UntaggedDouble::WriteTo(SnapshotWriter* writer,
                             intptr_t object_id,
                             Snapshot::Kind kind,
                             bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kDoubleCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the double value.
  writer->WriteDouble(value_);
}

template <typename StringType, typename CharacterType, typename CallbackType>
void String::ReadFromImpl(SnapshotReader* reader,
                          String* str_obj,
                          intptr_t len,
                          intptr_t tags,
                          CallbackType new_symbol,
                          Snapshot::Kind kind) {
  ASSERT(reader != NULL);
  if (UntaggedObject::IsCanonical(tags)) {
    // Set up canonical string object.
    ASSERT(reader != NULL);
    CharacterType* ptr = reader->zone()->Alloc<CharacterType>(len);
    for (intptr_t i = 0; i < len; i++) {
      ptr[i] = reader->Read<CharacterType>();
    }
    *str_obj = (*new_symbol)(reader->thread(), ptr, len);
  } else {
    // Set up the string object.
    *str_obj = StringType::New(len, Heap::kNew);
    str_obj->SetHash(0);  // Will get computed when needed.
    if (len == 0) {
      return;
    }
    NoSafepointScope no_safepoint;
    CharacterType* str_addr = StringType::DataStart(*str_obj);
    for (intptr_t i = 0; i < len; i++) {
      *str_addr = reader->Read<CharacterType>();
      str_addr++;
    }
  }
}

OneByteStringPtr OneByteString::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  // Read the length so that we can determine instance size to allocate.
  ASSERT(reader != NULL);
  intptr_t len = reader->ReadSmiValue();
  String& str_obj = String::ZoneHandle(reader->zone(), String::null());

  String::ReadFromImpl<OneByteString, uint8_t>(reader, &str_obj, len, tags,
                                               Symbols::FromLatin1, kind);
  reader->AddBackRef(object_id, &str_obj, kIsDeserialized);
  return raw(str_obj);
}

TwoByteStringPtr TwoByteString::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  // Read the length so that we can determine instance size to allocate.
  ASSERT(reader != NULL);
  intptr_t len = reader->ReadSmiValue();
  String& str_obj = String::ZoneHandle(reader->zone(), String::null());

  String::ReadFromImpl<TwoByteString, uint16_t>(reader, &str_obj, len, tags,
                                                Symbols::FromUTF16, kind);
  reader->AddBackRef(object_id, &str_obj, kIsDeserialized);
  return raw(str_obj);
}

template <typename T>
static void StringWriteTo(SnapshotWriter* writer,
                          intptr_t object_id,
                          Snapshot::Kind kind,
                          intptr_t class_id,
                          intptr_t tags,
                          SmiPtr length,
                          T* data) {
  ASSERT(writer != NULL);
  intptr_t len = Smi::Value(length);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(class_id);
  writer->WriteTags(tags);

  // Write out the length field.
  writer->Write<ObjectPtr>(length);

  // Write out the string.
  if (len > 0) {
    if (class_id == kOneByteStringCid) {
      writer->WriteBytes(reinterpret_cast<const uint8_t*>(data), len);
    } else {
      for (intptr_t i = 0; i < len; i++) {
        writer->Write(data[i]);
      }
    }
  }
}

void UntaggedOneByteString::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  StringWriteTo(writer, object_id, kind, kOneByteStringCid,
                writer->GetObjectTags(this), length(), data());
}

void UntaggedTwoByteString::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  StringWriteTo(writer, object_id, kind, kTwoByteStringCid,
                writer->GetObjectTags(this), length(), data());
}

ExternalOneByteStringPtr ExternalOneByteString::ReadFrom(SnapshotReader* reader,
                                                         intptr_t object_id,
                                                         intptr_t tags,
                                                         Snapshot::Kind kind,
                                                         bool as_reference) {
  UNREACHABLE();
  return ExternalOneByteString::null();
}

ExternalTwoByteStringPtr ExternalTwoByteString::ReadFrom(SnapshotReader* reader,
                                                         intptr_t object_id,
                                                         intptr_t tags,
                                                         Snapshot::Kind kind,
                                                         bool as_reference) {
  UNREACHABLE();
  return ExternalTwoByteString::null();
}

void UntaggedExternalOneByteString::WriteTo(SnapshotWriter* writer,
                                            intptr_t object_id,
                                            Snapshot::Kind kind,
                                            bool as_reference) {
  // Serialize as a non-external one byte string.
  StringWriteTo(writer, object_id, kind, kOneByteStringCid,
                writer->GetObjectTags(this), length(), external_data_);
}

void UntaggedExternalTwoByteString::WriteTo(SnapshotWriter* writer,
                                            intptr_t object_id,
                                            Snapshot::Kind kind,
                                            bool as_reference) {
  // Serialize as a non-external two byte string.
  StringWriteTo(writer, object_id, kind, kTwoByteStringCid,
                writer->GetObjectTags(this), length(), external_data_);
}

ArrayPtr Array::ReadFrom(SnapshotReader* reader,
                         intptr_t object_id,
                         intptr_t tags,
                         Snapshot::Kind kind,
                         bool as_reference) {
  ASSERT(reader != NULL);

  // Read the length so that we can determine instance size to allocate.
  intptr_t len = reader->ReadSmiValue();
  Array* array = NULL;
  DeserializeState state;
  if (!as_reference) {
    array = reinterpret_cast<Array*>(reader->GetBackRef(object_id));
    state = kIsDeserialized;
  } else {
    state = kIsNotDeserialized;
  }
  if (array == NULL) {
    array = &(Array::ZoneHandle(reader->zone(), Array::New(len)));
    reader->AddBackRef(object_id, array, state);
  }
  if (!as_reference) {
    // Read all the individual elements for inlined objects.
    ASSERT(!UntaggedObject::IsCanonical(tags));
    reader->ArrayReadFrom(object_id, *array, len, tags);
  }
  return array->ptr();
}

ImmutableArrayPtr ImmutableArray::ReadFrom(SnapshotReader* reader,
                                           intptr_t object_id,
                                           intptr_t tags,
                                           Snapshot::Kind kind,
                                           bool as_reference) {
  ASSERT(reader != NULL);

  // Read the length so that we can determine instance size to allocate.
  intptr_t len = reader->ReadSmiValue();
  Array* array = NULL;
  DeserializeState state;
  if (!as_reference) {
    array = reinterpret_cast<Array*>(reader->GetBackRef(object_id));
    state = kIsDeserialized;
  } else {
    state = kIsNotDeserialized;
  }
  if (array == NULL) {
    array = &(Array::ZoneHandle(reader->zone(), ImmutableArray::New(len)));
    reader->AddBackRef(object_id, array, state);
  }
  if (!as_reference) {
    // Read all the individual elements for inlined objects.
    reader->ArrayReadFrom(object_id, *array, len, tags);
    if (UntaggedObject::IsCanonical(tags)) {
      *array ^= array->Canonicalize(reader->thread());
    }
  }
  return raw(*array);
}

void UntaggedArray::WriteTo(SnapshotWriter* writer,
                            intptr_t object_id,
                            Snapshot::Kind kind,
                            bool as_reference) {
  ASSERT(!this->IsCanonical());
  writer->ArrayWriteTo(object_id, kArrayCid, writer->GetObjectTags(this),
                       length(), type_arguments(), data(), as_reference);
}

void UntaggedImmutableArray::WriteTo(SnapshotWriter* writer,
                                     intptr_t object_id,
                                     Snapshot::Kind kind,
                                     bool as_reference) {
  writer->ArrayWriteTo(object_id, kImmutableArrayCid,
                       writer->GetObjectTags(this), length(), type_arguments(),
                       data(), as_reference);
}

GrowableObjectArrayPtr GrowableObjectArray::ReadFrom(SnapshotReader* reader,
                                                     intptr_t object_id,
                                                     intptr_t tags,
                                                     Snapshot::Kind kind,
                                                     bool as_reference) {
  ASSERT(reader != NULL);

  // Read the length so that we can determine instance size to allocate.
  GrowableObjectArray& array = GrowableObjectArray::ZoneHandle(
      reader->zone(), GrowableObjectArray::null());
  array = GrowableObjectArray::New(0);
  reader->AddBackRef(object_id, &array, kIsDeserialized);

  // Read type arguments of growable array object.
  *reader->TypeArgumentsHandle() ^= reader->ReadObjectImpl(kAsInlinedObject);
  array.StoreCompressedPointer(&array.untag()->type_arguments_,
                               reader->TypeArgumentsHandle()->ptr());

  // Read length of growable array object.
  array.SetLength(reader->ReadSmiValue());

  // Read the backing array of growable array object.
  *(reader->ArrayHandle()) ^= reader->ReadObjectImpl(kAsReference);
  array.SetData(*(reader->ArrayHandle()));

  return array.ptr();
}

void UntaggedGrowableObjectArray::WriteTo(SnapshotWriter* writer,
                                          intptr_t object_id,
                                          Snapshot::Kind kind,
                                          bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kGrowableObjectArrayCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the type arguments field.
  writer->WriteObjectImpl(type_arguments(), kAsInlinedObject);

  // Write out the used length field.
  writer->Write<ObjectPtr>(length());

  // Write out the Array object.
  writer->WriteObjectImpl(data(), kAsReference);
}

LinkedHashMapPtr LinkedHashMap::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  ASSERT(reader != NULL);

  LinkedHashMap& map =
      LinkedHashMap::ZoneHandle(reader->zone(), LinkedHashMap::null());
  // Since the map might contain itself as a key or value, allocate first.
  map = LinkedHashMap::NewUninitialized();
  reader->AddBackRef(object_id, &map, kIsDeserialized);

  // Read the type arguments.
  *reader->TypeArgumentsHandle() ^= reader->ReadObjectImpl(kAsInlinedObject);
  map.SetTypeArguments(*reader->TypeArgumentsHandle());

  // Read the number of key/value pairs.
  intptr_t len = reader->ReadSmiValue();
  intptr_t used_data = (len << 1);
  map.SetUsedData(used_data);

  // Allocate the data array.
  intptr_t data_size =
      Utils::Maximum(Utils::RoundUpToPowerOfTwo(used_data),
                     static_cast<uintptr_t>(LinkedHashMap::kInitialIndexSize));
  Array& data = Array::ZoneHandle(reader->zone(), Array::New(data_size));
  map.SetData(data);
  map.SetDeletedKeys(0);

  // The index and hashMask is regenerated by the maps themselves on demand.
  // Thus, the index will probably be allocated in new space (unless it's huge).
  // TODO(koda): Eagerly rehash here when no keys have user-defined '==', and
  // in particular, if/when (const) maps are needed in the VM isolate snapshot.
  ASSERT(reader->isolate_group() != Dart::vm_isolate_group());
  map.SetHashMask(0);  // Prefer sentinel 0 over null for better type feedback.

  reader->EnqueueRehashingOfMap(map);

  // Read the keys and values.
  bool read_as_reference = UntaggedObject::IsCanonical(tags) ? false : true;
  for (intptr_t i = 0; i < used_data; i++) {
    *reader->PassiveObjectHandle() = reader->ReadObjectImpl(read_as_reference);
    data.SetAt(i, *reader->PassiveObjectHandle());
  }
  return map.ptr();
}

void UntaggedLinkedHashMap::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kLinkedHashMapCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the type arguments.
  writer->WriteObjectImpl(type_arguments(), kAsInlinedObject);

  const intptr_t num_used_data = Smi::Value(used_data());
  ASSERT((num_used_data & 1) == 0);  // Keys + values, so must be even.
  const intptr_t num_deleted_keys = Smi::Value(deleted_keys());

  // Write out the number of (not deleted) key/value pairs that will follow.
  writer->Write<ObjectPtr>(Smi::New((num_used_data >> 1) - num_deleted_keys));

  // Write out the keys and values.
  const bool write_as_reference = this->IsCanonical() ? false : true;
  ArrayPtr data_array = data();
  ASSERT(num_used_data <= Smi::Value(data_array->untag()->length()));
#if defined(DEBUG)
  intptr_t deleted_keys_found = 0;
#endif  // DEBUG
  for (intptr_t i = 0; i < num_used_data; i += 2) {
    ObjectPtr key = data_array->untag()->element(i);
    if (key == data_array) {
#if defined(DEBUG)
      ++deleted_keys_found;
#endif  // DEBUG
      continue;
    }
    ObjectPtr value = data_array->untag()->element(i + 1);
    writer->WriteObjectImpl(key, write_as_reference);
    writer->WriteObjectImpl(value, write_as_reference);
  }
  DEBUG_ASSERT(deleted_keys_found == num_deleted_keys);
}

Float32x4Ptr Float32x4::ReadFrom(SnapshotReader* reader,
                                 intptr_t object_id,
                                 intptr_t tags,
                                 Snapshot::Kind kind,
                                 bool as_reference) {
  ASSERT(reader != NULL);
  // Read the values.
  float value0 = reader->Read<float>();
  float value1 = reader->Read<float>();
  float value2 = reader->Read<float>();
  float value3 = reader->Read<float>();

  // Create a Float32x4 object.
  Float32x4& simd = Float32x4::ZoneHandle(reader->zone(), Float32x4::null());
  simd = Float32x4::New(value0, value1, value2, value3);
  reader->AddBackRef(object_id, &simd, kIsDeserialized);
  return simd.ptr();
}

void UntaggedFloat32x4::WriteTo(SnapshotWriter* writer,
                                intptr_t object_id,
                                Snapshot::Kind kind,
                                bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kFloat32x4Cid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the float values.
  writer->Write<float>(value_[0]);
  writer->Write<float>(value_[1]);
  writer->Write<float>(value_[2]);
  writer->Write<float>(value_[3]);
}

Int32x4Ptr Int32x4::ReadFrom(SnapshotReader* reader,
                             intptr_t object_id,
                             intptr_t tags,
                             Snapshot::Kind kind,
                             bool as_reference) {
  ASSERT(reader != NULL);
  // Read the values.
  uint32_t value0 = reader->Read<uint32_t>();
  uint32_t value1 = reader->Read<uint32_t>();
  uint32_t value2 = reader->Read<uint32_t>();
  uint32_t value3 = reader->Read<uint32_t>();

  // Create a Float32x4 object.
  Int32x4& simd = Int32x4::ZoneHandle(reader->zone(), Int32x4::null());
  simd = Int32x4::New(value0, value1, value2, value3);
  reader->AddBackRef(object_id, &simd, kIsDeserialized);
  return simd.ptr();
}

void UntaggedInt32x4::WriteTo(SnapshotWriter* writer,
                              intptr_t object_id,
                              Snapshot::Kind kind,
                              bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kInt32x4Cid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the mask values.
  writer->Write<uint32_t>(value_[0]);
  writer->Write<uint32_t>(value_[1]);
  writer->Write<uint32_t>(value_[2]);
  writer->Write<uint32_t>(value_[3]);
}

Float64x2Ptr Float64x2::ReadFrom(SnapshotReader* reader,
                                 intptr_t object_id,
                                 intptr_t tags,
                                 Snapshot::Kind kind,
                                 bool as_reference) {
  ASSERT(reader != NULL);
  // Read the values.
  double value0 = reader->Read<double>();
  double value1 = reader->Read<double>();

  // Create a Float64x2 object.
  Float64x2& simd = Float64x2::ZoneHandle(reader->zone(), Float64x2::null());
  simd = Float64x2::New(value0, value1);
  reader->AddBackRef(object_id, &simd, kIsDeserialized);
  return simd.ptr();
}

void UntaggedFloat64x2::WriteTo(SnapshotWriter* writer,
                                intptr_t object_id,
                                Snapshot::Kind kind,
                                bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kFloat64x2Cid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out the float values.
  writer->Write<double>(value_[0]);
  writer->Write<double>(value_[1]);
}

TypedDataPtr TypedData::ReadFrom(SnapshotReader* reader,
                                 intptr_t object_id,
                                 intptr_t tags,
                                 Snapshot::Kind kind,
                                 bool as_reference) {
  ASSERT(reader != NULL);

  intptr_t cid = UntaggedObject::ClassIdTag::decode(tags);
  intptr_t len = reader->ReadSmiValue();
  TypedData& result =
      TypedData::ZoneHandle(reader->zone(), TypedData::New(cid, len));
  reader->AddBackRef(object_id, &result, kIsDeserialized);

  // Setup the array elements.
  intptr_t element_size = ElementSizeInBytes(cid);
  intptr_t length_in_bytes = len * element_size;
  NoSafepointScope no_safepoint;
  uint8_t* data = reinterpret_cast<uint8_t*>(result.DataAddr(0));
  reader->Align(Zone::kAlignment);
  reader->ReadBytes(data, length_in_bytes);

  // If it is a canonical constant make it one.
  // When reading a full snapshot we don't need to canonicalize the object
  // as it would already be a canonical object.
  // When reading a script snapshot or a message snapshot we always have
  // to canonicalize the object.
  if (UntaggedObject::IsCanonical(tags)) {
    result ^= result.Canonicalize(reader->thread());
    ASSERT(!result.IsNull());
    ASSERT(result.IsCanonical());
  }
  return result.ptr();
}

ExternalTypedDataPtr ExternalTypedData::ReadFrom(SnapshotReader* reader,
                                                 intptr_t object_id,
                                                 intptr_t tags,
                                                 Snapshot::Kind kind,
                                                 bool as_reference) {
  ASSERT(!Snapshot::IsFull(kind));
  intptr_t cid = UntaggedObject::ClassIdTag::decode(tags);
  intptr_t length = reader->ReadSmiValue();

  FinalizableData finalizable_data =
      static_cast<MessageSnapshotReader*>(reader)->finalizable_data()->Take();
  uint8_t* data = reinterpret_cast<uint8_t*>(finalizable_data.data);
  ExternalTypedData& obj =
      ExternalTypedData::ZoneHandle(ExternalTypedData::New(cid, data, length));
  reader->AddBackRef(object_id, &obj, kIsDeserialized);
  intptr_t external_size = obj.LengthInBytes();
  obj.AddFinalizer(finalizable_data.peer, finalizable_data.callback,
                   external_size);
  return obj.ptr();
}

// This function's name can appear in Observatory.
static void IsolateMessageTypedDataFinalizer(void* isolate_callback_data,
                                             void* buffer) {
  free(buffer);
}

void UntaggedTypedData::WriteTo(SnapshotWriter* writer,
                                intptr_t object_id,
                                Snapshot::Kind kind,
                                bool as_reference) {
  ASSERT(writer != NULL);
  intptr_t cid = this->GetClassId();
  intptr_t length = Smi::Value(this->length());  // In elements.
  intptr_t external_cid;
  intptr_t bytes;
  switch (cid) {
    case kTypedDataInt8ArrayCid:
      external_cid = kExternalTypedDataInt8ArrayCid;
      bytes = length * sizeof(int8_t);
      break;
    case kTypedDataUint8ArrayCid:
      external_cid = kExternalTypedDataUint8ArrayCid;
      bytes = length * sizeof(uint8_t);
      break;
    case kTypedDataUint8ClampedArrayCid:
      external_cid = kExternalTypedDataUint8ClampedArrayCid;
      bytes = length * sizeof(uint8_t);
      break;
    case kTypedDataInt16ArrayCid:
      external_cid = kExternalTypedDataInt16ArrayCid;
      bytes = length * sizeof(int16_t);
      break;
    case kTypedDataUint16ArrayCid:
      external_cid = kExternalTypedDataUint16ArrayCid;
      bytes = length * sizeof(uint16_t);
      break;
    case kTypedDataInt32ArrayCid:
      external_cid = kExternalTypedDataInt32ArrayCid;
      bytes = length * sizeof(int32_t);
      break;
    case kTypedDataUint32ArrayCid:
      external_cid = kExternalTypedDataUint32ArrayCid;
      bytes = length * sizeof(uint32_t);
      break;
    case kTypedDataInt64ArrayCid:
      external_cid = kExternalTypedDataInt64ArrayCid;
      bytes = length * sizeof(int64_t);
      break;
    case kTypedDataUint64ArrayCid:
      external_cid = kExternalTypedDataUint64ArrayCid;
      bytes = length * sizeof(uint64_t);
      break;
    case kTypedDataFloat32ArrayCid:
      external_cid = kExternalTypedDataFloat32ArrayCid;
      bytes = length * sizeof(float);
      break;
    case kTypedDataFloat64ArrayCid:
      external_cid = kExternalTypedDataFloat64ArrayCid;
      bytes = length * sizeof(double);
      break;
    case kTypedDataInt32x4ArrayCid:
      external_cid = kExternalTypedDataInt32x4ArrayCid;
      bytes = length * sizeof(int32_t) * 4;
      break;
    case kTypedDataFloat32x4ArrayCid:
      external_cid = kExternalTypedDataFloat32x4ArrayCid;
      bytes = length * sizeof(float) * 4;
      break;
    case kTypedDataFloat64x2ArrayCid:
      external_cid = kExternalTypedDataFloat64x2ArrayCid;
      bytes = length * sizeof(double) * 2;
      break;
    default:
      external_cid = kIllegalCid;
      bytes = 0;
      UNREACHABLE();
  }

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  if ((kind == Snapshot::kMessage) &&
      (static_cast<uint64_t>(bytes) >= FLAG_externalize_typed_data_threshold)) {
    // Write as external.
    writer->WriteIndexedObject(external_cid);
    writer->WriteTags(writer->GetObjectTags(this));
    writer->Write<ObjectPtr>(this->length());
    uint8_t* data = reinterpret_cast<uint8_t*>(this->data());
    void* passed_data = malloc(bytes);
    memmove(passed_data, data, bytes);
    static_cast<MessageWriter*>(writer)->finalizable_data()->Put(
        bytes,
        passed_data,  // data
        passed_data,  // peer,
        IsolateMessageTypedDataFinalizer);
  } else {
    // Write as internal.
    writer->WriteIndexedObject(cid);
    writer->WriteTags(writer->GetObjectTags(this));
    writer->Write<ObjectPtr>(this->length());
    uint8_t* data = reinterpret_cast<uint8_t*>(this->data());
    writer->Align(Zone::kAlignment);
    writer->WriteBytes(data, bytes);
  }
}

void UntaggedExternalTypedData::WriteTo(SnapshotWriter* writer,
                                        intptr_t object_id,
                                        Snapshot::Kind kind,
                                        bool as_reference) {
  ASSERT(writer != NULL);
  intptr_t cid = this->GetClassId();
  intptr_t length = Smi::Value(this->length());  // In elements.
  intptr_t bytes;
  switch (cid) {
    case kExternalTypedDataInt8ArrayCid:
      bytes = length * sizeof(int8_t);
      break;
    case kExternalTypedDataUint8ArrayCid:
      bytes = length * sizeof(uint8_t);
      break;
    case kExternalTypedDataUint8ClampedArrayCid:
      bytes = length * sizeof(uint8_t);
      break;
    case kExternalTypedDataInt16ArrayCid:
      bytes = length * sizeof(int16_t);
      break;
    case kExternalTypedDataUint16ArrayCid:
      bytes = length * sizeof(uint16_t);
      break;
    case kExternalTypedDataInt32ArrayCid:
      bytes = length * sizeof(int32_t);
      break;
    case kExternalTypedDataUint32ArrayCid:
      bytes = length * sizeof(uint32_t);
      break;
    case kExternalTypedDataInt64ArrayCid:
      bytes = length * sizeof(int64_t);
      break;
    case kExternalTypedDataUint64ArrayCid:
      bytes = length * sizeof(uint64_t);
      break;
    case kExternalTypedDataFloat32ArrayCid:
      bytes = length * sizeof(float);  // NOLINT.
      break;
    case kExternalTypedDataFloat64ArrayCid:
      bytes = length * sizeof(double);  // NOLINT.
      break;
    case kExternalTypedDataInt32x4ArrayCid:
      bytes = length * sizeof(int32_t) * 4;
      break;
    case kExternalTypedDataFloat32x4ArrayCid:
      bytes = length * sizeof(float) * 4;
      break;
    case kExternalTypedDataFloat64x2ArrayCid:
      bytes = length * sizeof(double) * 2;
      break;
    default:
      bytes = 0;
      UNREACHABLE();
  }

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write as external.
  writer->WriteIndexedObject(cid);
  writer->WriteTags(writer->GetObjectTags(this));
  writer->Write<ObjectPtr>(this->length());
  uint8_t* data = reinterpret_cast<uint8_t*>(data_);
  void* passed_data = malloc(bytes);
  memmove(passed_data, data, bytes);
  static_cast<MessageWriter*>(writer)->finalizable_data()->Put(
      bytes,
      passed_data,  // data
      passed_data,  // peer,
      IsolateMessageTypedDataFinalizer);
}

void UntaggedTypedDataView::WriteTo(SnapshotWriter* writer,
                                    intptr_t object_id,
                                    Snapshot::Kind kind,
                                    bool as_reference) {
  // Views have always a backing store.
  ASSERT(typed_data() != Object::null());

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(GetClassId());
  writer->WriteTags(writer->GetObjectTags(this));

  // Write members.
  writer->Write<ObjectPtr>(offset_in_bytes());
  writer->Write<ObjectPtr>(length());
  writer->WriteObjectImpl(typed_data(), as_reference);
}

TypedDataViewPtr TypedDataView::ReadFrom(SnapshotReader* reader,
                                         intptr_t object_id,
                                         intptr_t tags,
                                         Snapshot::Kind kind,
                                         bool as_reference) {
  auto& typed_data = *reader->TypedDataBaseHandle();
  const classid_t cid = UntaggedObject::ClassIdTag::decode(tags);

  auto& view = *reader->TypedDataViewHandle();
  view = TypedDataView::New(cid);
  reader->AddBackRef(object_id, &view, kIsDeserialized);

  const intptr_t offset_in_bytes = reader->ReadSmiValue();
  const intptr_t length = reader->ReadSmiValue();
  typed_data ^= reader->ReadObjectImpl(as_reference);
  view.InitializeWith(typed_data, offset_in_bytes, length);

  return view.ptr();
}

CapabilityPtr Capability::ReadFrom(SnapshotReader* reader,
                                   intptr_t object_id,
                                   intptr_t tags,
                                   Snapshot::Kind kind,
                                   bool as_reference) {
  uint64_t id = reader->Read<uint64_t>();

  Capability& result =
      Capability::ZoneHandle(reader->zone(), Capability::New(id));
  reader->AddBackRef(object_id, &result, kIsDeserialized);
  return result.ptr();
}

void UntaggedCapability::WriteTo(SnapshotWriter* writer,
                                 intptr_t object_id,
                                 Snapshot::Kind kind,
                                 bool as_reference) {
  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kCapabilityCid);
  writer->WriteTags(writer->GetObjectTags(this));

  writer->Write<uint64_t>(id_);
}

SendPortPtr SendPort::ReadFrom(SnapshotReader* reader,
                               intptr_t object_id,
                               intptr_t tags,
                               Snapshot::Kind kind,
                               bool as_reference) {
  ASSERT(kind == Snapshot::kMessage);

  uint64_t id = reader->Read<uint64_t>();
  uint64_t origin_id = reader->Read<uint64_t>();

  SendPort& result =
      SendPort::ZoneHandle(reader->zone(), SendPort::New(id, origin_id));
  reader->AddBackRef(object_id, &result, kIsDeserialized);
  return result.ptr();
}

void UntaggedSendPort::WriteTo(SnapshotWriter* writer,
                               intptr_t object_id,
                               Snapshot::Kind kind,
                               bool as_reference) {
  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kSendPortCid);
  writer->WriteTags(writer->GetObjectTags(this));

  writer->Write<uint64_t>(id_);
  writer->Write<uint64_t>(origin_id_);
}

TransferableTypedDataPtr TransferableTypedData::ReadFrom(SnapshotReader* reader,
                                                         intptr_t object_id,
                                                         intptr_t tags,
                                                         Snapshot::Kind kind,
                                                         bool as_reference) {
  ASSERT(reader != nullptr);

  ASSERT(!Snapshot::IsFull(kind));
  const intptr_t length = reader->Read<int64_t>();

  const FinalizableData finalizable_data =
      static_cast<MessageSnapshotReader*>(reader)->finalizable_data()->Take();
  uint8_t* data = reinterpret_cast<uint8_t*>(finalizable_data.data);
  auto& transferableTypedData = TransferableTypedData::ZoneHandle(
      reader->zone(), TransferableTypedData::New(data, length));
  reader->AddBackRef(object_id, &transferableTypedData, kIsDeserialized);
  return transferableTypedData.ptr();
}

void UntaggedTransferableTypedData::WriteTo(SnapshotWriter* writer,
                                            intptr_t object_id,
                                            Snapshot::Kind kind,
                                            bool as_reference) {
  ASSERT(writer != nullptr);
  ASSERT(GetClassId() == kTransferableTypedDataCid);
  void* peer = writer->thread()->heap()->GetPeer(ObjectPtr(this));
  // Assume that object's Peer is only used to track transferrability state.
  ASSERT(peer != nullptr);
  TransferableTypedDataPeer* tpeer =
      reinterpret_cast<TransferableTypedDataPeer*>(peer);
  intptr_t length = tpeer->length();  // In bytes.
  void* data = tpeer->data();
  if (data == nullptr) {
    writer->SetWriteException(
        Exceptions::kArgument,
        "Illegal argument in isolate message"
        " : (TransferableTypedData has been transferred already)");
    return;
  }

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  writer->WriteIndexedObject(GetClassId());
  writer->WriteTags(writer->GetObjectTags(this));
  writer->Write<int64_t>(length);

  static_cast<MessageWriter*>(writer)->finalizable_data()->Put(
      length, data, tpeer,
      // Finalizer does nothing - in case of failure to serialize,
      // [data] remains wrapped in sender's [TransferableTypedData].
      [](void* data, void* peer) {},
      // This is invoked on successful serialization of the message
      [](void* data, void* peer) {
        TransferableTypedDataPeer* tpeer =
            reinterpret_cast<TransferableTypedDataPeer*>(peer);
        tpeer->handle()->EnsureFreedExternal(IsolateGroup::Current());
        tpeer->ClearData();
      });
}

RegExpPtr RegExp::ReadFrom(SnapshotReader* reader,
                           intptr_t object_id,
                           intptr_t tags,
                           Snapshot::Kind kind,
                           bool as_reference) {
  ASSERT(reader != NULL);
  // Allocate RegExp object.
  RegExp& regex =
      RegExp::ZoneHandle(reader->zone(), RegExp::New(reader->zone()));
  reader->AddBackRef(object_id, &regex, kIsDeserialized);

  // Read and Set all the other fields.
  *reader->ArrayHandle() ^= reader->ReadObjectImpl(kAsInlinedObject);
  regex.set_capture_name_map(*reader->ArrayHandle());
  *reader->StringHandle() ^= reader->ReadObjectImpl(kAsInlinedObject);
  regex.set_pattern(*reader->StringHandle());

  regex.StoreNonPointer(&regex.untag()->num_bracket_expressions_,
                        reader->Read<int32_t>());
  regex.StoreNonPointer(&regex.untag()->num_one_byte_registers_,
                        reader->Read<int32_t>());
  regex.StoreNonPointer(&regex.untag()->num_two_byte_registers_,
                        reader->Read<int32_t>());
  regex.StoreNonPointer(&regex.untag()->type_flags_, reader->Read<int8_t>());
  return regex.ptr();
}

void UntaggedRegExp::WriteTo(SnapshotWriter* writer,
                             intptr_t object_id,
                             Snapshot::Kind kind,
                             bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kRegExpCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the other fields.
  writer->WriteObjectImpl(capture_name_map(), kAsInlinedObject);
  writer->WriteObjectImpl(pattern(), kAsInlinedObject);
  writer->Write<int32_t>(num_bracket_expressions_);
  writer->Write<int32_t>(num_one_byte_registers_);
  writer->Write<int32_t>(num_two_byte_registers_);
  writer->Write<int8_t>(type_flags_);
}

WeakPropertyPtr WeakProperty::ReadFrom(SnapshotReader* reader,
                                       intptr_t object_id,
                                       intptr_t tags,
                                       Snapshot::Kind kind,
                                       bool as_reference) {
  ASSERT(reader != NULL);

  // Allocate the weak property object.
  WeakProperty& weak_property =
      WeakProperty::ZoneHandle(reader->zone(), WeakProperty::New());
  reader->AddBackRef(object_id, &weak_property, kIsDeserialized);

  // Set all the object fields.
  READ_COMPRESSED_OBJECT_FIELDS(
      weak_property, weak_property.ptr()->untag()->from(),
      weak_property.ptr()->untag()->to(), kAsReference);

  return weak_property.ptr();
}

void UntaggedWeakProperty::WriteTo(SnapshotWriter* writer,
                                   intptr_t object_id,
                                   Snapshot::Kind kind,
                                   bool as_reference) {
  ASSERT(writer != NULL);

  // Write out the serialization header value for this object.
  writer->WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  writer->WriteIndexedObject(kWeakPropertyCid);
  writer->WriteTags(writer->GetObjectTags(this));

  // Write out all the object pointer fields.
  SnapshotWriterVisitor visitor(writer, kAsReference);
  visitor.VisitCompressedPointers(heap_base(), from(), to());
}

}  // namespace dart
