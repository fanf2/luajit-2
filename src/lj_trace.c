/*
** Trace management.
** Copyright (C) 2005-2010 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_trace_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_gdbjit.h"
#include "lj_record.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_target.h"

/* -- Error handling ------------------------------------------------------ */

/* Synchronous abort with error message. */
void lj_trace_err(jit_State *J, TraceError e)
{
  setnilV(&J->errinfo);  /* No error info. */
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* Synchronous abort with error message and error info. */
void lj_trace_err_info(jit_State *J, TraceError e)
{
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* -- Trace management ---------------------------------------------------- */

/* The current trace is first assembled in J->cur. The variable length
** arrays point to shared, growable buffers (J->irbuf etc.). When trace
** recording ends successfully, the current trace and its data structures
** are copied to a new (compact) GCtrace object.
*/

/* Find a free trace number. */
static TraceNo trace_findfree(jit_State *J)
{
  MSize osz, lim;
  if (J->freetrace == 0)
    J->freetrace = 1;
  for (; J->freetrace < J->sizetrace; J->freetrace++)
    if (traceref(J, J->freetrace) == NULL)
      return J->freetrace++;
  /* Need to grow trace array. */
  lim = (MSize)J->param[JIT_P_maxtrace] + 1;
  if (lim < 2) lim = 2; else if (lim > 65535) lim = 65535;
  osz = J->sizetrace;
  if (osz >= lim)
    return 0;  /* Too many traces. */
  lj_mem_growvec(J->L, J->trace, J->sizetrace, lim, GCRef);
  for (; osz < J->sizetrace; osz++)
    setgcrefnull(J->trace[osz]);
  return J->freetrace;
}

#define TRACE_APPENDVEC(field, szfield, tp) \
  T->field = (tp *)p; \
  memcpy(p, J->cur.field, J->cur.szfield*sizeof(tp)); \
  p += J->cur.szfield*sizeof(tp);

#ifdef LUAJIT_USE_PERFTOOLS
/*
** Create symbol table of JIT-compiled code. For use with Linux perf tools.
** Example usage:
**   perf record -f -e cycles luajit test.lua
**   perf report -s symbol
**   rm perf.data /tmp/perf-*.map
*/
#include <stdio.h>
#include <unistd.h>

static void perftools_addtrace(GCtrace *T)
{
  static FILE *fp;
  GCproto *pt = &gcref(T->startpt)->pt;
  uintptr_t pcofs = (uintptr_t)(T->snap[0].mapofs+T->snap[0].nent);
  const BCIns *startpc = snap_pc(T->snapmap[pcofs]);
  const char *name = strdata(proto_chunkname(pt));
  BCLine lineno;
  if (name[0] == '@' || name[0] == '=')
    name++;
  else
    name = "(string)";
  if (startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc)
    lineno = proto_line(pt, proto_bcpos(pt, startpc));
  else
    lineno = proto_line(pt, 0);  /* Wrong, but better than nothing. */
  if (!fp) {
    char fname[40];
    sprintf(fname, "/tmp/perf-%d.map", getpid());
    if (!(fp = fopen(fname, "w"))) return;
    setlinebuf(fp);
  }
  fprintf(fp, "%lx %x TRACE_%d::%s:%u\n",
	  (long)T->mcode, T->szmcode, T->traceno, name, lineno);
}
#endif

/* Save current trace by copying and compacting it. */
static void trace_save(jit_State *J)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  size_t sz = sztr + szins +
	      J->cur.nsnap*sizeof(SnapShot) +
	      J->cur.nsnapmap*sizeof(SnapEntry);
  GCtrace *T = lj_mem_newt(J->L, (MSize)sz, GCtrace);
  char *p = (char *)T + sztr;
  memcpy(T, &J->cur, sizeof(GCtrace));
  setgcrefr(T->nextgc, J2G(J)->gc.root);
  setgcrefp(J2G(J)->gc.root, T);
  newwhite(J2G(J), T);
  T->gct = ~LJ_TTRACE;
  T->ir = (IRIns *)p - J->cur.nk;
  memcpy(p, J->cur.ir+J->cur.nk, szins);
  p += szins;
  TRACE_APPENDVEC(snap, nsnap, SnapShot)
  TRACE_APPENDVEC(snapmap, nsnapmap, SnapEntry)
  J->cur.traceno = 0;
  setgcrefp(J->trace[T->traceno], T);
  lj_gc_barriertrace(J2G(J), T->traceno);
  lj_gdbjit_addtrace(J, T);
#ifdef LUAJIT_USE_PERFTOOLS
  perftools_addtrace(T);
#endif
}

void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  if (T->traceno) {
    lj_gdbjit_deltrace(J, T);
    if (T->traceno < J->freetrace)
      J->freetrace = T->traceno;
    setgcrefnull(J->trace[T->traceno]);
  }
  lj_mem_free(g, T,
    ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
    T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry));
}

/* Re-enable compiling a prototype by unpatching any modified bytecode. */
void lj_trace_reenableproto(GCproto *pt)
{
  if ((pt->flags & PROTO_HAS_ILOOP)) {
    BCIns *bc = proto_bc(pt);
    BCPos i, sizebc = pt->sizebc;;
    pt->flags &= ~PROTO_HAS_ILOOP;
    if (bc_op(bc[0]) == BC_IFUNCF)
      setbc_op(&bc[0], BC_FUNCF);
    for (i = 1; i < sizebc; i++) {
      BCOp op = bc_op(bc[i]);
      if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP)
	setbc_op(&bc[i], (int)op+(int)BC_LOOP-(int)BC_ILOOP);
    }
  }
}

/* Unpatch the bytecode modified by a root trace. */
static void trace_unpatch(jit_State *J, GCtrace *T)
{
  BCOp op = bc_op(T->startins);
  MSize pcofs = T->snap[0].mapofs + T->snap[0].nent;
  BCIns *pc = ((BCIns *)snap_pc(T->snapmap[pcofs])) - 1;
  UNUSED(J);
  switch (op) {
  case BC_FORL:
    lua_assert(bc_op(*pc) == BC_JFORI);
    setbc_op(pc, BC_FORI);  /* Unpatch JFORI, too. */
    pc += bc_j(*pc);
    lua_assert(bc_op(*pc) == BC_JFORL && traceref(J, bc_d(*pc)) == T);
    *pc = T->startins;
    break;
  case BC_LOOP:
    lua_assert(bc_op(*pc) == BC_JLOOP && traceref(J, bc_d(*pc)) == T);
    *pc = T->startins;
    break;
  case BC_ITERL:
    lua_assert(bc_op(*pc) == BC_JMP);
    pc += bc_j(*pc)+2;
    lua_assert(bc_op(*pc) == BC_JITERL && traceref(J, bc_d(*pc)) == T);
    *pc = T->startins;
    break;
  case BC_FUNCF:
    lua_assert(bc_op(*pc) == BC_JFUNCF && traceref(J, bc_d(*pc)) == T);
    *pc = T->startins;
    break;
  case BC_JMP:  /* No need to unpatch branches in parent traces (yet). */
  default:
    lua_assert(0);
    break;
  }
}

/* Flush a root trace. */
static void trace_flushroot(jit_State *J, GCtrace *T)
{
  GCproto *pt = &gcref(T->startpt)->pt;
  lua_assert(T->root == 0 && pt != NULL);
  /* First unpatch any modified bytecode. */
  trace_unpatch(J, T);
  /* Unlink root trace from chain anchored in prototype. */
  if (pt->trace == T->traceno) {  /* Trace is first in chain. Easy. */
    pt->trace = T->nextroot;
  } else {  /* Otherwise search in chain of root traces. */
    GCtrace *T2 = traceref(J, pt->trace);
    while (T2->nextroot != T->traceno) {
      lua_assert(T2->nextroot != 0);
      T2 = traceref(J, T2->nextroot);
    }
    T2->nextroot = T->nextroot;  /* Unlink from chain. */
  }
}

/* Flush a trace. Only root traces are considered. */
void lj_trace_flush(jit_State *J, TraceNo traceno)
{
  if (traceno > 0 && traceno < J->sizetrace) {
    GCtrace *T = traceref(J, traceno);
    if (T && T->root == 0)
      trace_flushroot(J, T);
  }
}

/* Flush all traces associated with a prototype. */
void lj_trace_flushproto(global_State *g, GCproto *pt)
{
  while (pt->trace != 0)
    trace_flushroot(G2J(g), traceref(G2J(g), pt->trace));
}

/* Flush all traces. */
int lj_trace_flushall(lua_State *L)
{
  jit_State *J = L2J(L);
  ptrdiff_t i;
  if ((J2G(J)->hookmask & HOOK_GC))
    return 1;
  for (i = (ptrdiff_t)J->sizetrace-1; i > 0; i--) {
    GCtrace *T = traceref(J, i);
    if (T) {
      if (T->root == 0)
	trace_flushroot(J, T);
      lj_gdbjit_deltrace(J, T);
      T->traceno = 0;
      setgcrefnull(J->trace[i]);
    }
  }
  J->cur.traceno = 0;
  J->freetrace = 0;
  /* Free the whole machine code and invalidate all exit stub groups. */
  lj_mcode_free(J);
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "flush"));
  );
  return 0;
}

/* Initialize JIT compiler state. */
void lj_trace_initstate(global_State *g)
{
  jit_State *J = G2J(g);
  TValue *tv;
  /* Initialize SIMD constants. */
  tv = LJ_KSIMD(J, LJ_KSIMD_ABS);
  tv[0].u64 = U64x(7fffffff,ffffffff);
  tv[1].u64 = U64x(7fffffff,ffffffff);
  tv = LJ_KSIMD(J, LJ_KSIMD_NEG);
  tv[0].u64 = U64x(80000000,00000000);
  tv[1].u64 = U64x(80000000,00000000);
}

/* Free everything associated with the JIT compiler state. */
void lj_trace_freestate(global_State *g)
{
  jit_State *J = G2J(g);
#ifdef LUA_USE_ASSERT
  {  /* This assumes all traces have already been freed. */
    ptrdiff_t i;
    for (i = 1; i < (ptrdiff_t)J->sizetrace; i++)
      lua_assert(i == (ptrdiff_t)J->cur.traceno || traceref(J, i) == NULL);
  }
#endif
  lj_mcode_free(J);
  lj_ir_knum_freeall(J);
  lj_mem_freevec(g, J->snapmapbuf, J->sizesnapmap, SnapEntry);
  lj_mem_freevec(g, J->snapbuf, J->sizesnap, SnapShot);
  lj_mem_freevec(g, J->irbuf + J->irbotlim, J->irtoplim - J->irbotlim, IRIns);
  lj_mem_freevec(g, J->trace, J->sizetrace, GCRef);
}

/* -- Penalties and blacklisting ------------------------------------------ */

/* Blacklist a bytecode instruction. */
static void blacklist_pc(GCproto *pt, BCIns *pc)
{
  setbc_op(pc, (int)bc_op(*pc)+(int)BC_ILOOP-(int)BC_LOOP);
  pt->flags |= PROTO_HAS_ILOOP;
}

/* Penalize a bytecode instruction. */
static void penalty_pc(jit_State *J, GCproto *pt, BCIns *pc, TraceError e)
{
  uint32_t i, val = PENALTY_MIN;
  for (i = 0; i < PENALTY_SLOTS; i++)
    if (mref(J->penalty[i].pc, const BCIns) == pc) {  /* Cache slot found? */
      /* First try to bump its hotcount several times. */
      val = ((uint32_t)J->penalty[i].val << 1) +
	    LJ_PRNG_BITS(J, PENALTY_RNDBITS);
      if (val > PENALTY_MAX) {
	blacklist_pc(pt, pc);  /* Blacklist it, if that didn't help. */
	return;
      }
      goto setpenalty;
    }
  /* Assign a new penalty cache slot. */
  i = J->penaltyslot;
  J->penaltyslot = (J->penaltyslot + 1) & (PENALTY_SLOTS-1);
  setmref(J->penalty[i].pc, pc);
setpenalty:
  J->penalty[i].val = (uint16_t)val;
  J->penalty[i].reason = e;
  hotcount_set(J2GG(J), pc+1, val);
}

/* -- Trace compiler state machine ---------------------------------------- */

/* Start tracing. */
static void trace_start(jit_State *J)
{
  lua_State *L;
  TraceNo traceno;

  if ((J->pt->flags & PROTO_NO_JIT)) {  /* JIT disabled for this proto? */
    if (J->parent == 0) {
      /* Lazy bytecode patching to disable hotcount events. */
      lua_assert(bc_op(*J->pc) == BC_FORL || bc_op(*J->pc) == BC_ITERL ||
		 bc_op(*J->pc) == BC_LOOP || bc_op(*J->pc) == BC_FUNCF);
      setbc_op(J->pc, (int)bc_op(*J->pc)+(int)BC_ILOOP-(int)BC_LOOP);
      J->pt->flags |= PROTO_HAS_ILOOP;
    }
    J->state = LJ_TRACE_IDLE;  /* Silently ignored. */
    return;
  }

  /* Get a new trace number. */
  traceno = trace_findfree(J);
  if (LJ_UNLIKELY(traceno == 0)) {  /* No free trace? */
    lua_assert((J2G(J)->hookmask & HOOK_GC) == 0);
    lj_trace_flushall(J->L);
    J->state = LJ_TRACE_IDLE;  /* Silently ignored. */
    return;
  }
  setgcrefp(J->trace[traceno], &J->cur);

  /* Setup enough of the current trace to be able to send the vmevent. */
  memset(&J->cur, 0, sizeof(GCtrace));
  J->cur.traceno = traceno;
  J->cur.nins = J->cur.nk = REF_BASE;
  J->cur.ir = J->irbuf;
  J->cur.snap = J->snapbuf;
  J->cur.snapmap = J->snapmapbuf;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->bcskip = 0;
  J->guardemit.irt = 0;
  setgcref(J->cur.startpt, obj2gco(J->pt));

  L = J->L;
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "start"));
    setintV(L->top++, traceno);
    setfuncV(L, L->top++, J->fn);
    setintV(L->top++, proto_bcpos(J->pt, J->pc));
    if (J->parent) {
      setintV(L->top++, J->parent);
      setintV(L->top++, J->exitno);
    }
  );
  lj_record_setup(J);
}

/* Stop tracing. */
static void trace_stop(jit_State *J)
{
  BCIns *pc = (BCIns *)J->startpc;  /* Not const here. */
  BCOp op = bc_op(J->cur.startins);
  GCproto *pt = &gcref(J->cur.startpt)->pt;
  TraceNo traceno = J->cur.traceno;
  lua_State *L;

  switch (op) {
  case BC_FORL:
    setbc_op(pc+bc_j(J->cur.startins), BC_JFORI);  /* Patch FORI, too. */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
    /* Patch bytecode of starting instruction in root trace. */
    setbc_op(pc, (int)op+(int)BC_JLOOP-(int)BC_LOOP);
    setbc_d(pc, traceno);
  addroot:
    /* Add to root trace chain in prototype. */
    J->cur.nextroot = pt->trace;
    pt->trace = (TraceNo1)traceno;
    break;
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    *pc = BCINS_AD(BC_JLOOP, J->cur.snap[0].nslots, traceno);
    goto addroot;
  case BC_JMP:
    /* Patch exit branch in parent to side trace entry. */
    lua_assert(J->parent != 0 && J->cur.root != 0);
    lj_asm_patchexit(J, traceref(J, J->parent), J->exitno, J->cur.mcode);
    /* Avoid compiling a side trace twice (stack resizing uses parent exit). */
    traceref(J, J->parent)->snap[J->exitno].count = SNAPCOUNT_DONE;
    /* Add to side trace chain in root trace. */
    {
      GCtrace *root = traceref(J, J->cur.root);
      root->nchild++;
      J->cur.nextside = root->nextside;
      root->nextside = (TraceNo1)traceno;
    }
    break;
  default:
    lua_assert(0);
    break;
  }

  /* Commit new mcode only after all patching is done. */
  lj_mcode_commit(J, J->cur.mcode);
  trace_save(J);

  L = J->L;
  lj_vmevent_send(L, TRACE,
    setstrV(L, L->top++, lj_str_newlit(L, "stop"));
    setintV(L->top++, traceno);
  );
}

/* Start a new root trace for down-recursion. */
static int trace_downrec(jit_State *J)
{
  /* Restart recording at the return instruction. */
  lua_assert(J->pt != NULL);
  lua_assert(bc_isret(bc_op(*J->pc)));
  if (bc_op(*J->pc) == BC_RETM)
    return 0;  /* NYI: down-recursion with RETM. */
  J->parent = 0;
  J->exitno = 0;
  J->state = LJ_TRACE_RECORD;
  trace_start(J);
  return 1;
}

/* Abort tracing. */
static int trace_abort(jit_State *J)
{
  lua_State *L = J->L;
  TraceError e = LJ_TRERR_RECERR;
  TraceNo traceno;

  lj_mcode_abort(J);
  if (tvisnum(L->top-1))
    e = (TraceError)lj_num2int(numV(L->top-1));
  if (e == LJ_TRERR_MCODELM) {
    J->state = LJ_TRACE_ASM;
    return 1;  /* Retry ASM with new MCode area. */
  }
  /* Penalize or blacklist starting bytecode instruction. */
  if (J->parent == 0 && !bc_isret(bc_op(J->cur.startins)))
    penalty_pc(J, &gcref(J->cur.startpt)->pt, (BCIns *)J->startpc, e);

  /* Is there anything to abort? */
  traceno = J->cur.traceno;
  if (traceno) {
    ptrdiff_t errobj = savestack(L, L->top-1);  /* Stack may be resized. */
    J->cur.link = 0;
    lj_vmevent_send(L, TRACE,
      TValue *frame;
      const BCIns *pc;
      GCfunc *fn;
      setstrV(L, L->top++, lj_str_newlit(L, "abort"));
      setintV(L->top++, traceno);
      /* Find original Lua function call to generate a better error message. */
      frame = J->L->base-1;
      pc = J->pc;
      while (!isluafunc(frame_func(frame))) {
	pc = frame_pc(frame) - 1;
	frame = frame_prev(frame);
      }
      fn = frame_func(frame);
      setfuncV(L, L->top++, fn);
      setintV(L->top++, proto_bcpos(funcproto(fn), pc));
      copyTV(L, L->top++, restorestack(L, errobj));
      copyTV(L, L->top++, &J->errinfo);
    );
    /* Drop aborted trace after the vmevent (which may still access it). */
    setgcrefnull(J->trace[traceno]);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
    J->cur.traceno = 0;
  }
  L->top--;  /* Remove error object */
  if (e == LJ_TRERR_DOWNREC)
    return trace_downrec(J);
  else if (e == LJ_TRERR_MCODEAL)
    lj_trace_flushall(L);
  return 0;
}

/* Perform pending re-patch of a bytecode instruction. */
static LJ_AINLINE void trace_pendpatch(jit_State *J, int force)
{
  if (LJ_UNLIKELY(J->patchpc) && (force || J->chain[IR_RETF])) {
    *J->patchpc = J->patchins;
    J->patchpc = NULL;
  }
}

/* State machine for the trace compiler. Protected callback. */
static TValue *trace_state(lua_State *L, lua_CFunction dummy, void *ud)
{
  jit_State *J = (jit_State *)ud;
  UNUSED(dummy);
  do {
  retry:
    switch (J->state) {
    case LJ_TRACE_START:
      J->state = LJ_TRACE_RECORD;  /* trace_start() may change state. */
      trace_start(J);
      lj_dispatch_update(J2G(J));
      break;

    case LJ_TRACE_RECORD:
      trace_pendpatch(J, 0);
      setvmstate(J2G(J), RECORD);
      lj_vmevent_send(L, RECORD,
	setintV(L->top++, J->cur.traceno);
	setfuncV(L, L->top++, J->fn);
	setintV(L->top++, J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1);
	setintV(L->top++, J->framedepth);
      );
      lj_record_ins(J);
      break;

    case LJ_TRACE_END:
      trace_pendpatch(J, 1);
      J->loopref = 0;
      if ((J->flags & JIT_F_OPT_LOOP) &&
	  J->cur.link == J->cur.traceno && J->framedepth + J->retdepth == 0) {
	setvmstate(J2G(J), OPT);
	lj_opt_dce(J);
	if (lj_opt_loop(J)) {  /* Loop optimization failed? */
	  J->cur.link = 0;
	  J->loopref = J->cur.nins;
	  J->state = LJ_TRACE_RECORD;  /* Try to continue recording. */
	  break;
	}
	J->loopref = J->chain[IR_LOOP];  /* Needed by assembler. */
      }
      J->state = LJ_TRACE_ASM;
      break;

    case LJ_TRACE_ASM:
      setvmstate(J2G(J), ASM);
      lj_asm_trace(J, &J->cur);
      trace_stop(J);
      setvmstate(J2G(J), INTERP);
      J->state = LJ_TRACE_IDLE;
      lj_dispatch_update(J2G(J));
      return NULL;

    default:  /* Trace aborted asynchronously. */
      setintV(L->top++, (int32_t)LJ_TRERR_RECERR);
      /* fallthrough */
    case LJ_TRACE_ERR:
      trace_pendpatch(J, 1);
      if (trace_abort(J))
	goto retry;
      setvmstate(J2G(J), INTERP);
      J->state = LJ_TRACE_IDLE;
      lj_dispatch_update(J2G(J));
      return NULL;
    }
  } while (J->state > LJ_TRACE_RECORD);
  return NULL;
}

/* -- Event handling ------------------------------------------------------ */

/* A bytecode instruction is about to be executed. Record it. */
void lj_trace_ins(jit_State *J, const BCIns *pc)
{
  /* Note: J->L must already be set. pc is the true bytecode PC here. */
  J->pc = pc;
  J->fn = curr_func(J->L);
  J->pt = isluafunc(J->fn) ? funcproto(J->fn) : NULL;
  while (lj_vm_cpcall(J->L, NULL, (void *)J, trace_state) != 0)
    J->state = LJ_TRACE_ERR;
}

/* A hotcount triggered. Start recording a root trace. */
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc)
{
  /* Note: pc is the interpreter bytecode PC here. It's offset by 1. */
  hotcount_set(J2GG(J), pc, J->param[JIT_P_hotloop]+1);  /* Reset hotcount. */
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if (J->state == LJ_TRACE_IDLE &&
      !(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT))) {
    J->parent = 0;  /* Root trace. */
    J->exitno = 0;
    J->state = LJ_TRACE_START;
    lj_trace_ins(J, pc-1);
  }
}

/* Check for a hot side exit. If yes, start recording a side trace. */
static void trace_hotside(jit_State *J, const BCIns *pc)
{
  SnapShot *snap = &traceref(J, J->parent)->snap[J->exitno];
  if (!(J2G(J)->hookmask & (HOOK_GC|HOOK_VMEVENT)) &&
      snap->count != SNAPCOUNT_DONE &&
      ++snap->count >= J->param[JIT_P_hotexit]) {
    lua_assert(J->state == LJ_TRACE_IDLE);
    /* J->parent is non-zero for a side trace. */
    J->state = LJ_TRACE_START;
    lj_trace_ins(J, pc);
  }
}

/* Tiny struct to pass data to protected call. */
typedef struct ExitDataCP {
  jit_State *J;
  void *exptr;		/* Pointer to exit state. */
  const BCIns *pc;	/* Restart interpreter at this PC. */
} ExitDataCP;

/* Need to protect lj_snap_restore because it may throw. */
static TValue *trace_exit_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ExitDataCP *exd = (ExitDataCP *)ud;
  cframe_errfunc(L->cframe) = -1;  /* Inherit error function. */
  exd->pc = lj_snap_restore(exd->J, exd->exptr);
  UNUSED(dummy);
  return NULL;
}

/* A trace exited. Restore interpreter state. */
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
{
  lua_State *L = J->L;
  ExitDataCP exd;
  int errcode;
  const BCIns *pc;
  void *cf;
  exd.J = J;
  exd.exptr = exptr;
  errcode = lj_vm_cpcall(L, NULL, &exd, trace_exit_cp);
  if (errcode)
    return -errcode;  /* Return negated error code. */

  lj_vmevent_send(L, TEXIT,
    ExitState *ex = (ExitState *)exptr;
    uint32_t i;
    lj_state_checkstack(L, 4+RID_NUM_GPR+RID_NUM_FPR+LUA_MINSTACK);
    setintV(L->top++, J->parent);
    setintV(L->top++, J->exitno);
    setintV(L->top++, RID_NUM_GPR);
    setintV(L->top++, RID_NUM_FPR);
    for (i = 0; i < RID_NUM_GPR; i++)
      setnumV(L->top++, cast_num(ex->gpr[i]));
    for (i = 0; i < RID_NUM_FPR; i++) {
      setnumV(L->top, ex->fpr[i]);
      if (LJ_UNLIKELY(tvisnan(L->top)))
	setnanV(L->top);
      L->top++;
    }
  );

  pc = exd.pc;
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize)
    lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
  else
    trace_hotside(J, pc);
  if (bc_op(*pc) == BC_JLOOP) {
    BCIns *retpc = &traceref(J, bc_d(*pc))->startins;
    if (bc_isret(bc_op(*retpc))) {
      if (J->state == LJ_TRACE_RECORD) {
	J->patchins = *pc;
	J->patchpc = (BCIns *)pc;
	*J->patchpc = *retpc;
      } else {
	pc = retpc;
	setcframe_pc(cf, pc);
      }
    }
  }
  /* Return MULTRES or 0. */
  switch (bc_op(*pc)) {
  case BC_CALLM: case BC_CALLMT:
    return (int)((BCReg)(L->top - L->base) - bc_a(*pc) - bc_c(*pc));
  case BC_RETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc) - bc_d(*pc));
  case BC_TSETM:
    return (int)((BCReg)(L->top - L->base) + 1 - bc_a(*pc));
  default:
    if (bc_op(*pc) >= BC_FUNCF)
      return (int)((BCReg)(L->top - L->base) + 1);
    return 0;
  }
}

#endif
