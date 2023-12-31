// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Containers/ArrayView.h"
#include "Containers/StringView.h"
#include "Containers/BitArray.h"
#include "Containers/List.h"
#include "Misc/StringBuilder.h"
#include "Misc/MemStack.h"
#include "Misc/GeneratedTypeName.h"
#include "Hash/xxhash.h"
#include "HLSLTree/HLSLTreeTypes.h"
#include "HLSLTree/HLSLTreeHash.h"

class FMaterial;
class FMaterialCompilationOutput;
struct FStaticParameterSet;

namespace UE::Shader
{
class FPreshaderData;
}

/**
 * The HLSLTree module contains classes to build an HLSL AST (abstract syntax tree)
 * This allows C++ to procedurally define an HLSL program.  The structure of the tree is designed to be flexible, to facilitate incremental generation from a material node graph
 * Once the tree is complete, HLSL source code may be generated
 */
namespace UE::HLSLTree
{

class FNode;
class FScope;
class FStatement;
class FExpression;
class FFunction;
class FExpressionLocalPHI;
class FRequestedType;

class FEmitContext;
class FEmitScope;
class FEmitShaderExpression;

static constexpr int32 MaxNumPreviousScopes = 2;

/** Used to report any errors generating when preparing the HLSLTree */
class FErrorHandlerInterface
{
public:
	/** Recieves a list of owners for the node that generated the error, along with the error message */
	virtual void AddErrorInternal(TConstArrayView<UObject*> InOwners, FStringView InError) = 0;
};

class FNullErrorHandler final : public FErrorHandlerInterface
{
public:
	virtual void AddErrorInternal(TConstArrayView<UObject*> InOwners, FStringView InError) override
	{
	}
};

/** Root class of the HLSL AST */
class FNode
{
public:
	virtual ~FNode() {}

private:
	/** Next node in the FTree's list of all nodes */
	FNode* NextNode = nullptr;

	friend class FTree;
};

/**
 * OwnedNodes track 1 or more UObject 'owners'
 * When generating HLSLTree for materials, the owner will typically be the UMaterialExpression that created the node, or the UMaterial itself
 * Since certain nodes (FExpressions) are deduplicated and shared, it's possible to have multiple owners, if multiple owners attempt to create a node with the same parameters
 * Tracking owners is important for the following reasons:
 * - Attributing any generated errors to the owner(s)
 * - Tracking input/output types (to color wires/pins in the material editor for example)
 */
class FOwnedNode : public FNode
{
public:
	virtual TConstArrayView<UObject*> GetOwners() const = 0;
};

struct FEmitPreshaderScope
{
	FEmitPreshaderScope() = default;
	FEmitPreshaderScope(FEmitScope* InScope, const FExpression* InValue) : Scope(InScope), Value(InValue) {}

	FEmitScope* Scope = nullptr;
	const FExpression* Value = nullptr;
};

/**
 * Represents an HLSL statement.  This is a piece of code that doesn't evaluate to any value, but instead should be executed sequentially, and likely has side-effects.
 * Examples include assigning a value, or various control flow structures (if, for, while, etc)
 * This is an abstract base class, with derived classes representing various types of statements
 */
class FStatement : public FOwnedNode
{
public:
	UObject* GetOwner() const { return Owner; }
	FScope& GetParentScope() const { return *ParentScope; }

	virtual TConstArrayView<UObject*> GetOwners() const final { return MakeArrayView(&Owner, 1); }
	virtual bool IsLoop() const { return false; }

protected:
	virtual bool Prepare(FEmitContext& Context, FEmitScope& Scope) const = 0;
	virtual void EmitShader(FEmitContext& Context, FEmitScope& Scope) const;
	virtual void EmitPreshader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, TArrayView<const FEmitPreshaderScope> Scopes, Shader::FPreshaderData& OutPreshader) const;

private:
	/** The UObject that owns/created this node, allows errors generated by this node to be properly associated with the owner */
	UObject* Owner = nullptr;

	FScope* ParentScope = nullptr;

	friend class FTree;
	friend class FEmitContext;
	friend class FExpressionLocalPHI;
};

/**
 * FRequestedType is used when accessing various FExpression methods.  A requested type is represented by a concrete Shader::FType, along with a bit vector of components.  A bit set in this vector means the given component was requested.
 * This allows FExpressions to track access of particular components, which can enable various optimizations.  For example, a 'Swizzle' expression may only access the 'Y' component of its input.  So it might request a float2 value from
 * its input, but with only the second bit set.  This lets the input know that only the 'Y' component is needed for this request.  This is turn can be passed down the chain, which can allow certain expressions to be skipped or constant-folded.
 * This is especially useful when dealing with struct types, which are internally represented as a flattened list of components of all fields.  In many cases only certain fields of a struct type will be relevant, which can be tracked by
 * setting the bits associated with those fields.
 */
class FRequestedType
{
public:
	FRequestedType() = default;
	FRequestedType(FRequestedType&&) = default;
	FRequestedType(const FRequestedType&) = default;
	FRequestedType& operator=(FRequestedType&&) = default;
	FRequestedType& operator=(const FRequestedType&) = default;

	ENGINE_API FRequestedType(const Shader::FType& InType, bool bDefaultRequest = true);
	ENGINE_API FRequestedType(const FRequestedType& InType, bool bDefaultRequest);
	FRequestedType(Shader::EValueType InType, bool bDefaultRequest = true) : FRequestedType(Shader::FType(InType), bDefaultRequest) {}
	FRequestedType(const Shader::FStructType* InType, bool bDefaultRequest = true) : FRequestedType(Shader::FType(InType), bDefaultRequest) {}
	FRequestedType(const FName& InType, bool bDefaultRequest = true) : FRequestedType(Shader::FType(InType), bDefaultRequest) {}

	ENGINE_API Shader::EValueComponentType GetValueComponentType() const;

	bool IsComponentRequested(int32 Index) const
	{
		return Type.IsAny() || (RequestedComponents.IsValidIndex(Index) ? (bool)RequestedComponents[Index] : false);
	}

	/** Check if any of xyzw are requested, any of these components will request a numeric scalar type */
	bool IsNumericVectorRequested() const
	{
		return Type.IsAny() || FMath::IsWithin(RequestedComponents.Find(true), 0, 4);
	}

	bool IsVoid() const { return Type.IsVoid(); }
	bool IsStruct() const { return Type.IsStruct(); }
	bool IsObject() const { return Type.IsObject(); }
	bool IsNumeric() const { return Type.IsNumeric(); }

	/** No requested components */
	bool IsEmpty() const { return !Type.IsAny() && !RequestedComponents.Contains(true); }

	ENGINE_API void SetComponentRequest(int32 Index, bool bRequest = true);

	/** Marks the given field as requested (or not) */
	ENGINE_API void SetFieldRequested(const Shader::FStructField* Field, bool bRequest = true);
	void ClearFieldRequested(const Shader::FStructField* Field)
	{
		SetFieldRequested(Field, false);
	}

	/** Marks the given field as requested, based on the input request type (which should match the field type) */
	ENGINE_API void SetFieldRequested(const Shader::FStructField* Field, const FRequestedType& InRequest);

	/** Returns the requested type of the given field */
	ENGINE_API FRequestedType GetField(const Shader::FStructField* Field) const;

	/**
	 * The actual type that was requested.  Specific componets of this type are requested by setting RequestedComponents
	 * Shader::EValueType::Any is a valid type here, which means that RequestedComponents is ignored, and IsComponentRequested() will be true for any component
	 */
	Shader::FType Type;

	/** 1 bit per component, a value of 'true' means the specified component is requsted */
	TBitArray<> RequestedComponents;
};
inline bool operator==(const FRequestedType& Lhs, const FRequestedType& Rhs)
{
	return Lhs.Type == Rhs.Type && Lhs.RequestedComponents == Rhs.RequestedComponents;
}
inline bool operator!=(const FRequestedType& Lhs, const FRequestedType& Rhs)
{
	return !operator==(Lhs, Rhs);
}

inline void AppendHash(FHasher& Hasher, const FRequestedType& Value)
{
	AppendHash(Hasher, Value.Type);
	AppendHash(Hasher, Value.RequestedComponents);
}

/**
 * Prepared state for a single component.  Used by FPreparedType, which includes a list of these
 */
struct FPreparedComponent
{
	FPreparedComponent() = default;
	FPreparedComponent(EExpressionEvaluation InEvaluation) : Evaluation(InEvaluation) {}

	inline bool IsNone() const { return Evaluation == EExpressionEvaluation::None; }
	inline bool IsRequested() const { return IsRequestedEvaluation(Evaluation); }

	/**
	 * Get the evaluation of this component within the given scope
	 * See comment of 'LoopScope' for explanation of why evaluation may change due to scope
	 */
	EExpressionEvaluation GetEvaluation(const FEmitScope& Scope) const;

	void SetLoopEvaluation(FEmitScope& Scope);

	/**
	 * If 'Evaluation' is a loop evaluation, the loop status is only applied inside this scope.
	 * This is important for constant-folding and preshaders working with loops.  Consider something like this:
	 * int Value = 0;
	 * for(int Index = 0; Index < 10; ++Index)
	 * {
	 *     Value += Index;
	 * }
	 * In this example, 'Value' would have 'ConstantLoop' evaluation, with LoopScope set to the for-loop scope.  This means that *within* the scope of the loop,
	 * 'Value' is not constant, but it is constant *outside* the loop.  Outside of LoopScope, 'ConstantLoop' evaluation will switch to 'Constant'
	 */ 
	FEmitScope* LoopScope = nullptr;

	/** Numeric bounds of this component */
	Shader::FComponentBounds Bounds;

	/** Evaluation type of this component */
	EExpressionEvaluation Evaluation = EExpressionEvaluation::None;
};
inline bool operator==(const FPreparedComponent& Lhs, const FPreparedComponent& Rhs)
{
	return Lhs.Evaluation == Rhs.Evaluation &&
		Lhs.Bounds == Rhs.Bounds &&
		Lhs.LoopScope == Rhs.LoopScope;
}
inline bool operator!=(const FPreparedComponent& Lhs, const FPreparedComponent& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FPreparedComponent CombineComponents(const FPreparedComponent& Lhs, const FPreparedComponent& Rhs);

/**
 * Represents the prepared type of an FExpression.  Each FExpression will prepare a type during FExpression::PrepareValue().
 * The prepared type is represented by a concrete Shader::FType, along with a list of FPreparedComponents.
 * This structure matches FRequestedType, and in fact FRequestedType is often used as a 'mask' to only consider certain prepared components
 */
class FPreparedType
{
public:
	FPreparedType() = default;
	FPreparedType(const Shader::FType& InType, const FPreparedComponent& InComponent = FPreparedComponent());
	FPreparedType(Shader::EValueType InType, const FPreparedComponent& InComponent = FPreparedComponent()) : FPreparedType(Shader::FType(InType), InComponent) {}
	FPreparedType(const Shader::FStructType* InType, const FPreparedComponent& InComponent = FPreparedComponent()) : FPreparedType(Shader::FType(InType), InComponent) {}
	FPreparedType(const FName& InType, const FPreparedComponent& InComponent = FPreparedComponent()) : FPreparedType(Shader::FType(InType), InComponent) {}

	void SetEvaluation(EExpressionEvaluation Evaluation);
	void MergeEvaluation(EExpressionEvaluation Evaluation);
	void SetLoopEvaluation(FEmitScope& Scope, const FRequestedType& RequestedType);

	void SetField(const Shader::FStructField* Field, const FPreparedType& FieldType);
	FPreparedType GetFieldType(const Shader::FStructField* Field) const;

	/** Returns the number of components, only including components with valid evaluation */
	int32 GetNumPreparedComponents() const;

	/** For numeric vectors, the number of components in the result type will only include components that have valid evaluation */
	Shader::FType GetResultType() const;

	/** Converts to a requested type, based on IsRequestedEvaluation() */
	FRequestedType GetRequestedType() const;
	
	Shader::EValueComponentType GetValueComponentType() const;
	const TCHAR* GetName() const { return Type.GetName(); }
	bool IsVoid() const { return Type.IsVoid(); }
	bool IsStruct() const { return Type.IsStruct(); }
	bool IsObject() const { return Type.IsObject(); }
	bool IsNumeric() const { return Type.IsNumeric(); }
	bool IsNumericScalar() const { return Type.IsNumericScalar(); }

	/** Either void, or no prepared components */
	bool IsEmpty() const;

	EExpressionEvaluation GetEvaluation(const FEmitScope& Scope) const;
	EExpressionEvaluation GetEvaluation(const FEmitScope& Scope, const FRequestedType& RequestedType) const;
	EExpressionEvaluation GetFieldEvaluation(const FEmitScope& Scope, const FRequestedType& RequestedType, int32 ComponentIndex, int32 NumComponents) const;
	Shader::FComponentBounds GetBounds(const FRequestedType& RequestedType) const;
	FPreparedComponent GetMergedComponent() const;

	FPreparedComponent GetComponent(int32 Index) const;
	Shader::FComponentBounds GetComponentBounds(int32 Index) const;
	inline bool IsComponentRequested(int32 Index) const { return GetComponent(Index).IsRequested(); }

	void SetComponent(int32 Index, const FPreparedComponent& InComponent);
	void SetComponentBounds(int32 Index, const Shader::FComponentBounds Bounds);
	void MergeComponent(int32 Index, const FPreparedComponent& InComponent);

	void EnsureNumComponents(int32 NumComponents);

	/**
	 * The prepared type.  Status of components within this type can be configured by setting PreparedComponents
	 * Components within Type that don't have a coorisponding entry in PreparedComponents are considered to have ConstantZero evaluation
	 */
	Shader::FType Type;

	/** Evaluation type for each component, may be 'None' for components that are unused */
	TArray<FPreparedComponent, TInlineAllocator<4>> PreparedComponents;
};
inline bool operator==(const FPreparedType& Lhs, const FPreparedType& Rhs)
{
	return Lhs.Type == Rhs.Type && Lhs.PreparedComponents == Rhs.PreparedComponents;
}
inline bool operator!=(const FPreparedType& Lhs, const FPreparedType& Rhs)
{
	return !operator==(Lhs, Rhs);
}

FPreparedType MergePreparedTypes(const FPreparedType& Lhs, const FPreparedType& Rhs);
FPreparedType MakeNonLWCType(const FPreparedType& Type);

class FPrepareValueResult
{
public:
	const FPreparedType& GetPreparedType() const { return PreparedType; }

	bool SetTypeVoid();

	bool SetType(FEmitContext& Context, const FRequestedType& RequestedType, EExpressionEvaluation Evaluation, const Shader::FType& Type);
	bool SetType(FEmitContext& Context, const FRequestedType& RequestedType, const FPreparedType& Type);

private:
	bool TryMergePreparedType(FEmitContext& Context, const Shader::FType& Type);

	FPreparedType PreparedType;
	bool bPreparingValue = false;

	friend class FExpression;
	friend class FEmitContext;
};

struct FEmitValueShaderResult
{
	FEmitShaderExpression* Code = nullptr;
};

struct FEmitCustomHLSLParameterResult
{
	FStringBuilderBase* DeclarationCode = nullptr;
	FStringBuilderBase* ForwardCode = nullptr;
};

struct FEmitValuePreshaderResult
{
	explicit FEmitValuePreshaderResult(Shader::FPreshaderData& InPreshader) : Preshader(InPreshader) {}

	Shader::FPreshaderData& Preshader;
	Shader::FType Type;
};

enum class EDerivativeCoordinate : uint8
{
	Ddx,
	Ddy,
};

struct FExpressionDerivatives
{
	const FExpression* ExpressionDdx = nullptr;
	const FExpression* ExpressionDdy = nullptr;

	const FExpression* Get(EDerivativeCoordinate Coord) const { return (Coord == EDerivativeCoordinate::Ddx) ? ExpressionDdx : ExpressionDdy; }

	bool IsValid() const { return (bool)ExpressionDdx && (bool)ExpressionDdy; }
};

/**
 * Represents an HLSL expression.  This is a piece of code that evaluates to a value, but has no side effects.
 * Unlike statements, expressions are not expected to execute in any particular order.
 * Examples include constant literals, variable accessors, and various types of math operations
 * This is an abstract base class, with derived classes representing various types of expression
 * Derived expression classes should provide a constructor to fully initialize all fields.  All types given to this constructor must be hashable via HLSLTree::AppendHash().
 * A unique hash value will be generated for each expression, by hashing the constructor arguments, which will be used to deduplicate expressions
 */
class FExpression : public FOwnedNode
{
public:
	virtual TConstArrayView<UObject*> GetOwners() const final { return Owners; }

	/**
	 * Get Shader/Preshader/Constant value for the expression
	 * @param RequestedType the componets of the result that are needed
	 * @param PreparedType the prepared type of this expression, should be return of Context.PrepareExpression/GetPreparedType
	 * @param ResultType the requested output type, value will be cast to this type if needed
	 * Various overloads below will derive the missing values from those that are provided, and/or object PreparedType from the given Context directly
	 */
	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const FPreparedType& PreparedType, const Shader::FType& ResultType) const;
	Shader::FType GetValuePreshader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const FPreparedType& PreparedType, const Shader::FType& ResultType, Shader::FPreshaderData& OutPreshader) const;
	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const FPreparedType& PreparedType, const Shader::FType& ResultType) const;

	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const Shader::FType& ResultType) const;
	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType) const;
	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope, const Shader::FType& ResultType) const;
	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope, Shader::EValueType ResultType) const;
	FEmitShaderExpression* GetValueShader(FEmitContext& Context, FEmitScope& Scope) const;

	Shader::FType GetValuePreshader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const Shader::FType& ResultType, Shader::FPreshaderData& OutPreshader) const;
	Shader::FType GetValuePreshader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, Shader::FPreshaderData& OutPreshader) const;
	Shader::FType GetValuePreshader(FEmitContext& Context, FEmitScope& Scope, const Shader::FType& ResultType, Shader::FPreshaderData& OutPreshader) const;
	Shader::FType GetValuePreshader(FEmitContext& Context, FEmitScope& Scope, Shader::EValueType ResultType, Shader::FPreshaderData& OutPreshader) const;

	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const FPreparedType& PreparedType) const;
	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, const Shader::FType& ResultType) const;
	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType) const;
	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FPreparedType& PreparedType, const Shader::FType& ResultType) const;
	Shader::FValue GetValueConstant(FEmitContext& Context, FEmitScope& Scope, const FPreparedType& PreparedType, Shader::EValueType ResultType) const;

	/**
	 * Gets the value as an 'Object' with a certain type
	 * Individual expressions will potentially support different types of objects
	 * @param ObjectTypeName should be the object type that matches the type returned by PrepareExpression
	 * @param OutObjectBase pointer to an object of the correct type
	 * @return true if given type is supported, otherwise false
	 */
	bool GetValueObject(FEmitContext& Context, FEmitScope& Scope, const FName& ObjectTypeName, void* OutObjectBase) const;

	template<typename ObjectType>
	bool GetValueObject(FEmitContext& Context, FEmitScope& Scope, ObjectType& OutObject) const
	{
		return GetValueObject(Context, Scope, ObjectType::GetTypeName(), &OutObject);
	}

	/**
	 * Some expressions may support passing object types to custom HLSL expressions
	 * CheckObjectSupportsCustomHLSL() will return true if this is supported
	 * If supported, GetObjectCustomHLSLParameter() will generate code to declare the parameters in the custom HLSL function, and forward the parameters
	 * EmitValueShader() will be called to generate the actual HLSL code for the expression
	 */
	bool CheckObjectSupportsCustomHLSL(FEmitContext& Context, FEmitScope& Scope, const FName& ObjectTypeName) const;
	void GetObjectCustomHLSLParameter(FEmitContext& Context,
		FEmitScope& Scope,
		const FName& ObjectTypeName,
		const TCHAR* ParameterName,
		FStringBuilderBase& OutDeclarationCode,
		FStringBuilderBase& OutForwardCode) const;

protected:
	/** Create new expressions representing DDX/DDY of this expression */
	virtual void ComputeAnalyticDerivatives(FTree& Tree, FExpressionDerivatives& OutResult) const;

	/** Creates a new expression representing this expression on the previous frame.  By default returns nullptr, which means the previous frame is the same as the current frame */
	virtual const FExpression* ComputePreviousFrame(FTree& Tree, const FRequestedType& RequestedType) const;

	/**
	 * Computes a FPreparedType for this expression, given a FRequestedType.  Will be called multiple times, with potentially different requested types, if the FExpression is used multiple times.
	 * In this case, all the prepared types will be merged together if possible (or generate an error otherwise).
	 * PrepareValue will be called in two phases, first with Context.bMarkLiveValues=false, then with bMarkLiveValues=true.
	 * If Context.bMarkLiveValues is true, the expression should record any state to signal to the client that it's active.
	 * For example, a material expression parameter might add the parameter name or texture value to the material data.  This is typically done through the Context.FindData() interface.
	 * If Context.bMarkLiveValues is true, the expression should also take care *not* to call Context.PrepareExpression for any nested expressions that are not relevant to the final result.
	 * For example, in a multiply expression where one input is 0, the other input is not relevant, and can be skipped.
	 * The EExpressionEvaluation set by this is important for determining which EmitValue*** methods may be called
	 * A given FExpression implementation must support at least one of EmitValueShader/EmitValuePreshader, but doesn't need to support both
	 * TODO? - It feels messy to have PrepareValue() used for both of these phases, possibly better to have a separate MarkLive() virtual method...in practice that would often lead to duplicate code however
	 */
	virtual bool PrepareValue(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, FPrepareValueResult& OutResult) const = 0;

	/** Emit HLSL shader code representing this expression */
	virtual void EmitValueShader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, FEmitValueShaderResult& OutResult) const;

	/** Emit Preshader code representing this expression */
	virtual void EmitValuePreshader(FEmitContext& Context, FEmitScope& Scope, const FRequestedType& RequestedType, FEmitValuePreshaderResult& OutResult) const;

	/** Emit an object.  The given 'ObjectTypeName' determines the C++ type pointed to by OutObjectBase */
	virtual bool EmitValueObject(FEmitContext& Context, FEmitScope& Scope, const FName& ObjectTypeName, void* OutObjectBase) const;

	/**
	 * Allows custom objects to be passed to custom HLSL functions.  This needs to initialize some HLSL code that facilitates this interface.
	 * If this returns 'true', then EmitValueShader() will be called to generate the actual HLSL code for the FExpression
	 */
	virtual bool EmitCustomHLSLParameter(FEmitContext& Context, FEmitScope& Scope, const FName& ObjectTypeName, const TCHAR* ParameterName, FEmitCustomHLSLParameterResult& OutResult) const;

private:
	TArray<UObject*, TInlineAllocator<2>> Owners;

	friend class FTree;
	friend class FEmitContext;
	friend class FExpressionForward;
	friend class FExpressionFunctionCall;
	friend class FExpressionOperation;
};

/**
 * Functions are a way to dynamically inject new scopes into the tree
 * They are used to represent calls to material functions using execution flow, and to inject dynamic branches without explicit control flow
 */
class FFunction final : public FNode
{
public:
	FScope& GetRootScope() const { return *RootScope; }

	/** The root scope of the function */
	FScope* RootScope = nullptr;

	/** The scope where this function is called, when HLSL is emitted, the RootScope will be injected here */
	FScope* CalledScope = nullptr;

	/** Function output expressions, should be expressions nested under RootScope */
	TArray<const FExpression*, TInlineAllocator<8>> OutputExpressions;
};

/**
 * Represents an HLSL scope.  A scope contains a single statement, along with any expressions required by that statement
 */
class FScope final : public FNode
{
public:
	static FScope* FindSharedParent(FScope* Lhs, FScope* Rhs);

	inline FScope* GetParentScope() const { return ParentScope; }

	inline TArrayView<FScope*> GetPreviousScopes() const
	{
		// const_cast needed, otherwise type of array view is 'FScope*const' which doesn't make sense
		return MakeArrayView(const_cast<FScope*>(this)->PreviousScope, NumPreviousScopes);
	}

	bool HasParentScope(const FScope& ParentScope) const;

	void AddPreviousScope(FScope& Scope);


private:
	friend class FTree;
	friend class FExpression;
	friend class FEmitContext;

	FScope* ParentScope = nullptr;
	FStatement* OwnerStatement = nullptr;
	FStatement* ContainedStatement = nullptr;
	FScope* PreviousScope[MaxNumPreviousScopes];
	TMap<FName, const FExpression*> LocalMap;
	int32 NumPreviousScopes = 0;
	int32 NestedLevel = 0;
};

/**
 * The HLSL AST.  Basically a wrapper around the root scope, with some helper methods
 */
class FTree
{
public:
	static FTree* Create(FMemStackBase& Allocator);
	static void Destroy(FTree* Tree);

	FMemStackBase& GetAllocator() { return *Allocator; }

	bool Finalize();

	void PushOwner(UObject* Owner);
	UObject* PopOwner();
	UObject* GetCurrentOwner() const;

	bool EmitShader(FEmitContext& Context, FStringBuilderBase& OutCode) const;

	FScope& GetRootScope() const { return *RootScope; }

	/**
	 * Creates a new FExpression-derived type by passing the given arguments to the constructor
	 * Given arguments are hashed, and may result in returning an existing expression if the hash matches
	 */
	template<typename T, typename... ArgTypes>
	inline const FExpression* NewExpression(ArgTypes&&... Args)
	{
		FHasher Hasher;
		AppendHash(Hasher, GetGeneratedTypeName<T>());
		AppendHashes(Hasher, Forward<ArgTypes>(Args)...);
		const FXxHash64 Hash = Hasher.Finalize();
		FExpression* Expression = FindExpression(Hash);
		if (!Expression)
		{
			T* TypedExpression = NewNode<T>(Forward<ArgTypes>(Args)...);
			RegisterExpression(TypedExpression, Hash);
			Expression = TypedExpression;
		}
		else
		{
			AddCurrentOwner(Expression);
		}
		return Expression;
	}

	template<typename T, typename... ArgTypes>
	inline T* NewStatement(FScope& Scope, ArgTypes&&... Args)
	{
		T* Statement = NewNode<T>(Forward<ArgTypes>(Args)...);
		RegisterStatement(Scope, Statement);
		return Statement;
	}

	void AssignLocal(FScope& Scope, const FName& LocalName, const FExpression* Value);
	const FExpression* AcquireLocal(FScope& Scope, const FName& LocalName);

	const FExpression* NewFunctionCall(FScope& Scope, FFunction* Function, int32 OutputIndex);

	FExpressionDerivatives GetAnalyticDerivatives(const FExpression* InExpression);
	const FExpression* GetPreviousFrame(const FExpression* InExpression, const FRequestedType& RequestedType);

	FScope* NewScope(FScope& Scope);
	FScope* NewOwnedScope(FStatement& Owner);
	FFunction* NewFunction();

	/** Shortcuts to create various common expression types */
	const FExpression* NewConstant(const Shader::FValue& Value);
	const FExpression* NewSwizzle(const FSwizzleParameters& Params, const FExpression* Input);
	const FExpression* NewUnaryOp(EOperation Op, const FExpression* Input);
	const FExpression* NewBinaryOp(EOperation Op, const FExpression* Lhs, const FExpression* Rhs);
	const FExpression* NewTernaryOp(EOperation Op, const FExpression* Input0, const FExpression* Input1, const FExpression* Input2);

	const FExpression* NewAbs(const FExpression* Input) { return NewUnaryOp(EOperation::Abs, Input); }
	const FExpression* NewNeg(const FExpression* Input) { return NewUnaryOp(EOperation::Neg, Input); }
	const FExpression* NewSaturate(const FExpression* Input) { return NewUnaryOp(EOperation::Saturate, Input); }
	const FExpression* NewSum(const FExpression* Input) { return NewUnaryOp(EOperation::Sum, Input); }
	const FExpression* NewRcp(const FExpression* Input) { return NewUnaryOp(EOperation::Rcp, Input); }
	const FExpression* NewSqrt(const FExpression* Input) { return NewUnaryOp(EOperation::Sqrt, Input); }
	const FExpression* NewRsqrt(const FExpression* Input) { return NewUnaryOp(EOperation::Rsqrt, Input); }
	const FExpression* NewLog2(const FExpression* Input) { return NewUnaryOp(EOperation::Log2, Input); }
	const FExpression* NewExp(const FExpression* Input) { return NewUnaryOp(EOperation::Exp, Input); }
	const FExpression* NewExp2(const FExpression* Input) { return NewUnaryOp(EOperation::Exp2, Input); }
	const FExpression* NewFrac(const FExpression* Input) { return NewUnaryOp(EOperation::Frac, Input); }
	const FExpression* NewLength(const FExpression* Input) { return NewUnaryOp(EOperation::Length, Input); }
	const FExpression* NewNormalize(const FExpression* Input) { return NewUnaryOp(EOperation::Normalize, Input); }
	const FExpression* NewSin(const FExpression* Input) { return NewUnaryOp(EOperation::Sin, Input); }
	const FExpression* NewCos(const FExpression* Input) { return NewUnaryOp(EOperation::Cos, Input); }

	const FExpression* NewAdd(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Add, Lhs, Rhs); }
	const FExpression* NewSub(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Sub, Lhs, Rhs); }
	const FExpression* NewMul(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Mul, Lhs, Rhs); }
	const FExpression* NewDiv(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Div, Lhs, Rhs); }
	const FExpression* NewFmod(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Fmod, Lhs, Rhs); }
	const FExpression* NewStep(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Step, Lhs, Rhs); }
	const FExpression* NewPowClamped(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::PowPositiveClamped, Lhs, Rhs); }
	const FExpression* NewMin(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Min, Lhs, Rhs); }
	const FExpression* NewMax(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Max, Lhs, Rhs); }
	const FExpression* NewLess(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Less, Lhs, Rhs); }
	const FExpression* NewGreater(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::Greater, Lhs, Rhs); }
	const FExpression* NewLessEqual(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::LessEqual, Lhs, Rhs); }
	const FExpression* NewGreaterEqual(const FExpression* Lhs, const FExpression* Rhs) { return NewBinaryOp(EOperation::GreaterEqual, Lhs, Rhs); }

	const FExpression* NewSmoothStep(const FExpression* Input0, const FExpression* Input1, const FExpression* Input2) { return NewTernaryOp(EOperation::SmoothStep, Input0,	Input1, Input2); }

	const FExpression* NewLog(const FExpression* Input);
	const FExpression* NewPow2(const FExpression* Input) { return NewMul(Input, Input); }
	const FExpression* NewCross(const FExpression* Lhs, const FExpression* Rhs);
	const FExpression* NewDot(const FExpression* Lhs, const FExpression* Rhs) { return NewSum(NewMul(Lhs, Rhs)); }
	const FExpression* NewLerp(const FExpression* A, const FExpression* B, const FExpression* T) { return NewAdd(A, NewMul(NewSub(B, A), T)); }

	const FExpression* NewTruncateLWC(const FExpression* Input) { return NewUnaryOp(EOperation::TruncateLWC, Input); }

private:
	template<typename T, typename... ArgTypes>
	inline T* NewNode(ArgTypes&&... Args)
	{
		T* Node = new(*Allocator) T(Forward<ArgTypes>(Args)...);
		RegisterNode(Node);
		return Node;
	}

	void RegisterNode(FNode* Node);
	void RegisterExpression(FExpression* Expression, FXxHash64 Hash);
	void RegisterExpression(FExpressionLocalPHI* Expression, FXxHash64 Hash);
	void AddCurrentOwner(FExpression* Expression);
	void RegisterStatement(FScope& Scope, FStatement* Statement);
	FExpression* FindExpression(FXxHash64 Hash);

	FMemStackBase* Allocator = nullptr;
	FNode* Nodes = nullptr;
	FScope* RootScope = nullptr;
	TArray<UObject*, TInlineAllocator<8>> OwnerStack;
	TMap<FXxHash64, FExpression*> ExpressionMap;
	TArray<FExpressionLocalPHI*> PHIExpressions;

	TMap<const FExpression*, FExpressionDerivatives> ExpressionDerivativesMap;
	TMap<FXxHash64, const FExpression*> PreviousFrameExpressionMap;

	friend class FExpressionLocalPHI;
};

struct FOwnerScope : private FNoncopyable
{
	FOwnerScope(FTree& InTree, UObject* InOwner, bool bPushOwner = true) : Tree(bPushOwner ? &InTree : nullptr), Owner(InOwner)
	{
		if (bPushOwner)
		{
			Tree->PushOwner(InOwner);
		}
	}

	~FOwnerScope()
	{
		if (Tree)
		{
			verify(Tree->PopOwner() == Owner);
		}
	}

	FTree* Tree;
	UObject* Owner;
};

} // namespace UE::HLSLTree

#endif // WITH_EDITOR
