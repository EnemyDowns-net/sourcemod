// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2006-2015 AlliedModders LLC
//
// This file is part of SourcePawn. SourcePawn is free software: you can
// redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// You should have received a copy of the GNU General Public License along with
// SourcePawn. If not, see http://www.gnu.org/licenses/.
//

#include <stdio.h>
#include <string.h>
#include "NativeInvoker.h"

/********************
* FUNCTION CALLING *
********************/

NativeInvoker::NativeInvoker(IPluginContext *pContext, const ke::RefPtr<Native> &native)
 : context_(pContext),
   m_curparam(0),
   m_errorstate(SP_ERROR_NONE),
   native_(native)
{
}

NativeInvoker::~NativeInvoker()
{
  Cancel();
}

bool
NativeInvoker::IsRunnable()
{
  return true;
}

IPluginContext *
NativeInvoker::GetParentContext()
{
  return context_;
}

int NativeInvoker::PushCell(cell_t cell)
{
  if (m_curparam >= SP_MAX_EXEC_PARAMS)
    return SetError(SP_ERROR_PARAMS_MAX);

  m_info[m_curparam].marked = false;
  m_params[m_curparam] = cell;
  m_curparam++;

  return SP_ERROR_NONE;
}

int
NativeInvoker::PushCellByRef(cell_t *cell, int flags)
{
  return PushArray(cell, 1, flags);
}

int
NativeInvoker::PushFloat(float number)
{
  cell_t val = sp::FloatCellUnion(number).cell;

  return PushCell(val);
}

int
NativeInvoker::PushFloatByRef(float *number, int flags)
{
  return PushCellByRef((cell_t *)number, flags);
}

int
NativeInvoker::PushArray(cell_t *inarray, unsigned int cells, int copyback)
{
  if (m_curparam >= SP_MAX_EXEC_PARAMS)
  {
    return SetError(SP_ERROR_PARAMS_MAX);
  }

  ParamInfo *info = &m_info[m_curparam];

  info->flags = inarray ? copyback : 0;
  info->marked = true;
  info->size = cells;
  info->str.is_sz = false;
  info->orig_addr = inarray;

  m_curparam++;

  return SP_ERROR_NONE;
}

int
NativeInvoker::PushString(const char *string)
{
  return _PushString(string, SM_PARAM_STRING_COPY, 0, strlen(string)+1);
}

int
NativeInvoker::PushStringEx(char *buffer, size_t length, int sz_flags, int cp_flags)
{
  return _PushString(buffer, sz_flags, cp_flags, length);
}

int
NativeInvoker::_PushString(const char *string, int sz_flags, int cp_flags, size_t len)
{
  if (m_curparam >= SP_MAX_EXEC_PARAMS)
    return SetError(SP_ERROR_PARAMS_MAX);

  ParamInfo *info = &m_info[m_curparam];

  info->marked = true;
  info->orig_addr = (cell_t *)string;
  info->flags = cp_flags;
  info->size = len;
  info->str.sz_flags = sz_flags;
  info->str.is_sz = true;

  m_curparam++;

  return SP_ERROR_NONE;
}

void
NativeInvoker::Cancel()
{
  if (!m_curparam)
    return;

  m_errorstate = SP_ERROR_NONE;
  m_curparam = 0;
}

int
NativeInvoker::Execute(cell_t *result, cell_t buffer, cell_t size)
{
  context_->ClearLastNativeError();

  // For backward compatibility, we have to clear the exception state.
  // Otherwise code like this:
  //
  // static cell_t native(cx, params) {
  //   for (auto callback : callbacks) {
  //     callback->Execute();
  //   }
  // }
  //
  // Could unintentionally leak a pending exception back to the caller,
  // which wouldn't have happened before the Great Exception Refactoring.

  SourcePawn::ExceptionHandler eh(context_);
  eh.Debug(!size);

  if (!Invoke(result)) {
    if(size)
      context_->StringToLocalUTF8(buffer, size, eh.Message(), NULL);
    int Err = context_->GetLastNativeError();
    context_->ClearLastNativeError();
    return Err;
  }

  return SP_ERROR_NONE;
}

bool
NativeInvoker::Invoke(cell_t *result)
{
  if (!IsRunnable()) {
    Cancel();
    context_->ReportErrorNumber(SP_ERROR_NOT_RUNNABLE);
    return false;
  }
  if (int err = m_errorstate) {
    Cancel();
    context_->ReportErrorNumber(err);
    return false;
  }

  //This is for re-entrancy!
  cell_t _temp_params[SP_MAX_EXEC_PARAMS + 1];
  cell_t *temp_params = &_temp_params[1];
  ParamInfo temp_info[SP_MAX_EXEC_PARAMS];
  unsigned int numparams = m_curparam;
  unsigned int i;

  if (numparams)
  {
    //Save the info locally, then reset it for re-entrant calls.
    memcpy(temp_info, m_info, numparams * sizeof(ParamInfo));
  }
  m_curparam = 0;

  /* Initialize 0th parameter */
  _temp_params[0] = numparams;

  /* Browse the parameters and build arrays */
  bool ok = true;
  for (i=0; i<numparams; i++) {
    /* Is this marked as an array? */
    if (temp_info[i].marked) {
      if (!temp_info[i].str.is_sz) {
        /* Allocate a normal/generic array */
        int err = context_->HeapAlloc(
          temp_info[i].size,
          &(temp_info[i].local_addr),
          &(temp_info[i].phys_addr));
        if (err != SP_ERROR_NONE) {
          context_->ReportErrorNumber(err);
          ok = false;
          break;
        }
        if (temp_info[i].orig_addr)
        {
          memcpy(temp_info[i].phys_addr, temp_info[i].orig_addr, sizeof(cell_t) * temp_info[i].size);
        }
      } else {
        /* Calculate cells required for the string */
        size_t cells = (temp_info[i].size + sizeof(cell_t) - 1) / sizeof(cell_t);

        /* Allocate the buffer */
        int err = context_->HeapAlloc(
          cells,
          &(temp_info[i].local_addr),
          &(temp_info[i].phys_addr));
        if (err != SP_ERROR_NONE) {
          context_->ReportErrorNumber(err);
          ok = false;
          break;
        }

        /* Copy original string if necessary */
        if ((temp_info[i].str.sz_flags & SM_PARAM_STRING_COPY) && (temp_info[i].orig_addr != NULL))
        {
          /* Cut off UTF-8 properly */
          if (temp_info[i].str.sz_flags & SM_PARAM_STRING_UTF8) {
            context_->StringToLocalUTF8(
              temp_info[i].local_addr,
              temp_info[i].size,
              (const char *)temp_info[i].orig_addr,
              NULL);
          }
          /* Copy a binary blob */
          else if (temp_info[i].str.sz_flags & SM_PARAM_STRING_BINARY)
          {
            memmove(temp_info[i].phys_addr, temp_info[i].orig_addr, temp_info[i].size);
          }
          /* Copy ASCII characters */
          else
          {
            context_->StringToLocal(
              temp_info[i].local_addr,
              temp_info[i].size,
              (const char *)temp_info[i].orig_addr);
          }
        }
      } /* End array/string calculation */
      /* Update the pushed parameter with the byref local address */
      temp_params[i] = temp_info[i].local_addr;
    } else {
      /* Just copy the value normally */
      temp_params[i] = m_params[i];
    }
  }

  /* Make the call if we can */
  if (ok)
  {
    *result = native_->func()(context_, _temp_params);
  }

  /* i should be equal to the last valid parameter + 1 */
  bool docopies = ok;
  while (i--) {
    if (!temp_info[i].marked)
      continue;

    if (docopies && (temp_info[i].flags & SM_PARAM_COPYBACK)) {
      if (temp_info[i].orig_addr) {
        if (temp_info[i].str.is_sz) {
          memcpy(temp_info[i].orig_addr, temp_info[i].phys_addr, temp_info[i].size);

        } else {
          if (temp_info[i].size == 1) {
            *temp_info[i].orig_addr = *(temp_info[i].phys_addr);
          } else {
            memcpy(temp_info[i].orig_addr,
                temp_info[i].phys_addr,
                temp_info[i].size * sizeof(cell_t));
          }
        }
      }
    }

    if (int err = context_->HeapPop(temp_info[i].local_addr))
      context_->ReportErrorNumber(err);
  }

  return context_->GetLastNativeError() == SP_ERROR_NONE;
}

int
NativeInvoker::SetError(int err)
{
  m_errorstate = err;

  return err;
}

int NativeInvoker::CallFunction(const cell_t *params, unsigned int num_params, cell_t *result) { return 0; }
funcid_t NativeInvoker::GetFunctionID() { return 0; }
int NativeInvoker::Execute2(IPluginContext *ctx, cell_t *result) { return 0; }
int NativeInvoker::CallFunction2(IPluginContext *ctx, const cell_t *params, unsigned int num_params, cell_t *result) { return 0; }
IPluginRuntime *NativeInvoker::GetParentRuntime() { return NULL; }
