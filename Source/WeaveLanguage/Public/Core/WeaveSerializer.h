#pragma once

#include "CoreMinimal.h"

struct FWeaveAST;

class WEAVELANGUAGE_API FWeaveSerializer
{
public:
	static FString Serialize(const FWeaveAST& AST);
};
