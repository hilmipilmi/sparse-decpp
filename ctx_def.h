#ifndef CTX_DEF_SPARSE_H
#define CTX_DEF_SPARSE_H

struct sparse_ctx;

#define USE_CTX
#undef  USE_CTX

#ifdef  USE_CTX

#define SCTX_ struct sparse_ctx *_sctx, 
#define SCTX  struct sparse_ctx *_sctx

#define sctx_ _sctx,
#define sctx  _sctx
#define sctxp _sctx->

#define SCTXCNT 1
#define DO_CTX

#define SPARSE_CTX_INIT struct sparse_ctx __sctx; struct sparse_ctx *_sctx = sparse_ctx_init(&__sctx);

extern struct sparse_ctx *sparse_ctx_init(struct sparse_ctx *);

#else

#define SCTX_  
#define SCTX  void

#define sctx_ 
#define sctx  
#define sctxp 

#define SCTXCNT 0
#undef  DO_CTX

#define SPARSE_CTX_INIT ;

#endif

#endif