#pragma once

template<typename T>
inline void GstSafeDestroy(T*& Obj)
{
	if (Obj)
	{
		Obj->Destroy();
		Obj = nullptr;
	}
}
