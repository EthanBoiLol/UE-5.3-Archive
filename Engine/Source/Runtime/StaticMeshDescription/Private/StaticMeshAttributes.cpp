// Copyright Epic Games, Inc. All Rights Reserved.

#include "StaticMeshAttributes.h"

namespace MeshAttribute
{
	const FName VertexInstance::TextureCoordinate("TextureCoordinate");
	const FName VertexInstance::Normal("Normal");
	const FName VertexInstance::Tangent("Tangent");
	const FName VertexInstance::BinormalSign("BinormalSign");
	const FName VertexInstance::Color("Color");

	const FName Edge::IsHard("IsHard");

	const FName Triangle::Normal("Normal");
	const FName Triangle::Tangent("Tangent");
	const FName Triangle::Binormal("Binormal");

	const FName Polygon::Normal("Normal");
	const FName Polygon::Tangent("Tangent");
	const FName Polygon::Binormal("Binormal");
	const FName Polygon::Center("Center");

	const FName PolygonGroup::ImportedMaterialSlotName("ImportedMaterialSlotName");

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	// all deprected in 4.26
	const FName Vertex::CornerSharpness("CornerSharpness");
	const FName PolygonGroup::CastShadow("CastShadow");
	const FName Edge::CreaseSharpness("CreaseSharpness");
	const FName Edge::IsUVSeam("IsUVSeam");
	const FName PolygonGroup::EnableCollision("EnableCollision");
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}



void FStaticMeshAttributes::Register(bool bKeepExistingAttribute)
{
	// Add basic vertex attributes

	// Add basic vertex instance attributes
	if (!MeshDescription.VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::TextureCoordinate) || !bKeepExistingAttribute)
	{
		MeshDescription.VertexInstanceAttributes().RegisterAttribute<FVector2f>(MeshAttribute::VertexInstance::TextureCoordinate, 1, FVector2f::ZeroVector, EMeshAttributeFlags::Lerpable | EMeshAttributeFlags::Mandatory);
	}

	if (!MeshDescription.VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Normal) || !bKeepExistingAttribute)
	{
		MeshDescription.VertexInstanceAttributes().RegisterAttribute<FVector3f>(MeshAttribute::VertexInstance::Normal, 1, FVector3f::ZeroVector, EMeshAttributeFlags::AutoGenerated | EMeshAttributeFlags::Mandatory);
	}

	if (!MeshDescription.VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Tangent) || !bKeepExistingAttribute)
	{
		MeshDescription.VertexInstanceAttributes().RegisterAttribute<FVector3f>(MeshAttribute::VertexInstance::Tangent, 1, FVector3f::ZeroVector, EMeshAttributeFlags::AutoGenerated | EMeshAttributeFlags::Mandatory);
	}

	if (!MeshDescription.VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::BinormalSign) || !bKeepExistingAttribute)
	{
		MeshDescription.VertexInstanceAttributes().RegisterAttribute<float>(MeshAttribute::VertexInstance::BinormalSign, 1, 0.0f, EMeshAttributeFlags::AutoGenerated | EMeshAttributeFlags::Mandatory);
	}

	if (!MeshDescription.VertexInstanceAttributes().HasAttribute(MeshAttribute::VertexInstance::Color) || !bKeepExistingAttribute)
	{
		MeshDescription.VertexInstanceAttributes().RegisterAttribute<FVector4f>(MeshAttribute::VertexInstance::Color, 1, FVector4f(1.0f, 1.0f, 1.0f, 1.0f), EMeshAttributeFlags::Lerpable | EMeshAttributeFlags::Mandatory);
	}

	// Add basic edge attributes
	if (!MeshDescription.EdgeAttributes().HasAttribute(MeshAttribute::Edge::IsHard) || !bKeepExistingAttribute)
	{
		MeshDescription.EdgeAttributes().RegisterAttribute<bool>(MeshAttribute::Edge::IsHard, 1, false, EMeshAttributeFlags::Mandatory);
	}

	// Add basic polygon attributes

	// Add basic polygon group attributes
	if (!MeshDescription.PolygonGroupAttributes().HasAttribute(MeshAttribute::PolygonGroup::ImportedMaterialSlotName) || !bKeepExistingAttribute)
	{
		MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 1, NAME_None, EMeshAttributeFlags::Mandatory); //The unique key to match the mesh material slot
	}

	// Call super class
	FMeshAttributes::Register(bKeepExistingAttribute);
}


void FStaticMeshAttributes::RegisterPolygonNormalAndTangentAttributes()
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	MeshDescription.PolygonAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Polygon::Normal, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	MeshDescription.PolygonAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Polygon::Tangent, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	MeshDescription.PolygonAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Polygon::Binormal, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	MeshDescription.PolygonAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Polygon::Center, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}


void FStaticMeshAttributes::RegisterTriangleNormalAndTangentAttributes()
{
	MeshDescription.TriangleAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Triangle::Normal, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	MeshDescription.TriangleAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Triangle::Tangent, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
	MeshDescription.TriangleAttributes().RegisterAttribute<FVector3f>(MeshAttribute::Triangle::Binormal, 1, FVector3f::ZeroVector, EMeshAttributeFlags::Transient);
}
