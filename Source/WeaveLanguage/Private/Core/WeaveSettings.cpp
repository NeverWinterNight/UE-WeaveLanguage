#include "Core/WeaveSettings.h"

UWeaverSettings::UWeaverSettings()
{
}

UWeaverSettings* UWeaverSettings::Get()
{
	return GetMutableDefault<UWeaverSettings>();
}
