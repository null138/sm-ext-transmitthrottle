#pragma once

#include "smsdk_ext.h"
#include <stdint.h>
#include "IForwardSys.h"
#include "sp_vm_api.h"

class TransmitThrottle : public SDKExtension, public IConCommandBaseAccessor
{
public:
	virtual bool SDK_OnLoad(char *error, size_t maxlength, bool late);
	virtual void SDK_OnUnload();
	virtual void SDK_OnAllLoaded();
	virtual bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	virtual bool RegisterConCommandBase(ConCommandBase *pVar) {
		return META_REGCVAR(pVar);
	}
};

extern TransmitThrottle g_TransmitThrottle;
extern IShareSys *sharesys;
extern IExtension *myself;