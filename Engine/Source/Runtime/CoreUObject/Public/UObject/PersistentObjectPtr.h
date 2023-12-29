// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PersistentObjectPtr.h: Template that is a base class for Lazy and Asset pointers
=============================================================================*/

#pragma once

#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

/**
 * TPersistentObjectPtr is a template base class for FLazyObjectPtr and FSoftObjectPtr
 */
template<class TObjectID>
struct TPersistentObjectPtr
{
	using ElementType = TObjectID;

	/** Default constructor, will be null */
	FORCEINLINE TPersistentObjectPtr()
	{
		Reset();
	}

	/** Reset the lazy pointer back to the null state */
	FORCEINLINE void Reset()
	{
		WeakPtr.Reset();
		ObjectID.Reset();
	}

	/** Resets the weak ptr only, call this when ObjectId may change */
	FORCEINLINE void ResetWeakPtr()
	{
		WeakPtr.Reset();
	}

	/** Construct from a unique object identifier */
	explicit FORCEINLINE TPersistentObjectPtr(const TObjectID& InObjectID)
		: WeakPtr()
		, ObjectID(InObjectID)
	{
	}

	/** Copy from a unique object identifier */
	FORCEINLINE void operator=(const TObjectID& InObjectID)
	{
		WeakPtr.Reset();
		ObjectID = InObjectID;
	}

	/** Copy from an object pointer */
	FORCEINLINE void operator=(const class UObject* Object)
	{
		if (Object)
		{
			ObjectID = TObjectID::GetOrCreateIDForObject(Object);
			WeakPtr = Object;
		}
		else
		{
			Reset();
		}
	}
	
	/** Copy from an existing weak pointer, reserve IDs if required */
	FORCEINLINE void operator=(const FWeakObjectPtr& Other)
	{
		// If object exists need to make sure it gets registered properly in above function, if it doesn't exist just empty it
		const UObject* Object = Other.Get();
		*this = Object;
	}

	/**
	 * Gets the unique object identifier associated with this lazy pointer. Valid even if pointer is not currently valid
	 *
	 * @return Unique ID for this object, or an invalid FUniqueObjectGuid if this pointer isn't set to anything
	 */
	FORCEINLINE const TObjectID& GetUniqueID() const
	{
		return ObjectID;
	}

	/** Non-const version of the above */
	FORCEINLINE TObjectID& GetUniqueID()
	{
		return ObjectID;
	}

	/**
	 * Dereference the pointer, which may cause it to become valid again. Will not try to load pending outside of game thread
	 *
	 * @return nullptr if this object is gone or the pointer was null, otherwise a valid UObject pointer
	 */
	FORCEINLINE UObject* Get() const
	{
		UObject* Object = WeakPtr.Get();
		
		// Do a full resolve if the cached object is null but we have a valid object ID that might resolve
		// This used to check TObjectID::GetCurrentTag() before resolving but that was unreliable and did not improve performance in actual use
		if (!Object && ObjectID.IsValid())
		{
			Object = ObjectID.ResolveObject();
			WeakPtr = Object;

			// Make sure it isn't garbage to match the default behavior of WeakPtr.Get() without looking it up again
			return ::GetValid(Object);
		}
		return Object;
	}

	/**
	 * Dereference the lazy pointer, which may cause it to become valid again. Will not try to load pending outside of game thread
	 *
	 * @param bEvenIfPendingKill, if this is true, pendingkill objects are considered valid
	 * @return nullptr if this object is gone or the lazy pointer was null, otherwise a valid UObject pointer
	 */
	FORCEINLINE UObject* Get(bool bEvenIfPendingKill) const
	{
		UObject* Object = WeakPtr.Get(bEvenIfPendingKill);

		// Do a full resolve if the cached object is null but we have a valid object ID that might resolve
		// This used to check TObjectID::GetCurrentTag() before resolving but that was unreliable and did not improve performance in actual use
		if (!Object && ObjectID.IsValid())
		{
			Object = ObjectID.ResolveObject();
			WeakPtr = Object;

			// Get the object again using the correct flag
			Object = WeakPtr.Get(bEvenIfPendingKill);
		}
		return Object;
	}

	/** Dereference the pointer */
	FORCEINLINE class UObject& operator*() const
	{
		return *Get();
	}

	/** Dereference the pointer */
	FORCEINLINE class UObject* operator->() const
	{
		return Get();
	}

	/** Compare pointers for equality. Only Serial Number matters for the base implementation */
	FORCEINLINE friend bool operator==(const TPersistentObjectPtr& Lhs, const TPersistentObjectPtr& Rhs)
	{
		return Lhs.ObjectID == Rhs.ObjectID;
	}

	FORCEINLINE friend bool operator==(const TPersistentObjectPtr& Lhs, TYPE_OF_NULLPTR)
	{
		return !Lhs.IsValid();
	}

	FORCEINLINE friend bool operator==(TYPE_OF_NULLPTR, const TPersistentObjectPtr& Rhs)
	{
		return !Rhs.IsValid();
	}

	/** Compare pointers for inequality. Only Serial Number matters for the base implementation */
	FORCEINLINE friend bool operator!=(const TPersistentObjectPtr& Lhs, const TPersistentObjectPtr& Rhs)
	{
		return Lhs.ObjectID != Rhs.ObjectID;
	}

	FORCEINLINE friend bool operator!=(const TPersistentObjectPtr& Lhs, TYPE_OF_NULLPTR)
	{
		return Lhs.IsValid();
	}

	FORCEINLINE friend bool operator!=(TYPE_OF_NULLPTR, const TPersistentObjectPtr& Rhs)
	{
		return Rhs.IsValid();
	}

	/**  
	 * Test if this does not point to a live UObject, but may in the future
	 * 
	 * @return true if this does not point to a real object, but could possibly
	 */
	FORCEINLINE bool IsPending() const
	{
		return Get() == nullptr && ObjectID.IsValid();
	}

	/**  
	 * Test if this points to a live UObject
	 *
	 * @return true if Get() would return a valid non-null pointer
	 */
	FORCEINLINE bool IsValid() const
	{
		return !!Get();
	}

	/**  
	 * Slightly different than !IsValid(), returns true if this used to point to a UObject, but doesn't any more and has not been assigned or reset in the mean time.
	 *
	 * @return true if this used to point at a real object but no longer does.
	 */
	FORCEINLINE bool IsStale() const
	{
		return WeakPtr.IsStale();
	}
	/**  
	 * Test if this can never point to a live UObject
	 *
	 * @return true if this is explicitly pointing to no object
	 */
	FORCEINLINE bool IsNull() const
	{
		return !ObjectID.IsValid();
	}

	/** Hash function */
	FORCEINLINE friend uint32 GetTypeHash(const TPersistentObjectPtr& Ptr)
	{
		return GetTypeHash(Ptr.ObjectID);
	}

private:

	/** Once the object has been noticed to be loaded, this is set to the object weak pointer **/
	mutable FWeakObjectPtr	WeakPtr;
	/** Guid for the object this pointer points to or will point to. **/
	TObjectID				ObjectID;
};

template <class TObjectID> struct TIsPODType<TPersistentObjectPtr<TObjectID> > { enum { Value = TIsPODType<TObjectID>::Value }; };
template <class TObjectID> struct TIsWeakPointerType<TPersistentObjectPtr<TObjectID> > { enum { Value = TIsWeakPointerType<FWeakObjectPtr>::Value }; };

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_2
#include "CoreMinimal.h"
#endif
