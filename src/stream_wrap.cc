// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "stream_wrap.h"
#include "stream_base-inl.h"

#include "env-inl.h"
#include "handle_wrap.h"
#include "node_buffer.h"
#include "node_counters.h"
#include "pipe_wrap.h"
#include "req_wrap-inl.h"
#include "tcp_wrap.h"
#include "udp_wrap.h"
#include "util-inl.h"

#include <stdlib.h>  // abort()
#include <string.h>  // memcpy()
#include <limits.h>  // INT_MAX


namespace node {

using v8::Context;
using v8::DontDelete;
using v8::EscapableHandleScope;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Local;
using v8::Object;
using v8::ReadOnly;
using v8::Signature;
using v8::Value;


void LibuvStreamWrap::Initialize(Local<Object> target,
                                 Local<Value> unused,
                                 Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);

  auto is_construct_call_callback =
      [](const FunctionCallbackInfo<Value>& args) {
    CHECK(args.IsConstructCall());
    ClearWrap(args.This());
  };
  // 新建一个函数模板
  Local<FunctionTemplate> sw =
      FunctionTemplate::New(env->isolate(), is_construct_call_callback);
  sw->InstanceTemplate()->SetInternalFieldCount(1);
  Local<String> wrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "ShutdownWrap");
  sw->SetClassName(wrapString);
  AsyncWrap::AddWrapMethods(env, sw);
  // 注册ShutdownWrap变量
  target->Set(wrapString, sw->GetFunction());

  Local<FunctionTemplate> ww =
      FunctionTemplate::New(env->isolate(), is_construct_call_callback);
  ww->InstanceTemplate()->SetInternalFieldCount(1);
  Local<String> writeWrapString =
      FIXED_ONE_BYTE_STRING(env->isolate(), "WriteWrap");
  ww->SetClassName(writeWrapString);
  AsyncWrap::AddWrapMethods(env, ww);
  // 注册WriteWrap变量
  target->Set(writeWrapString, ww->GetFunction());
  env->set_write_wrap_constructor_function(ww->GetFunction());
}


LibuvStreamWrap::LibuvStreamWrap(Environment* env,
                                 Local<Object> object,
                                 uv_stream_t* stream,
                                 AsyncWrap::ProviderType provider)
    : HandleWrap(env,
                 object,
                 reinterpret_cast<uv_handle_t*>(stream),
                 provider),
      StreamBase(env),
      stream_(stream) {
}


void LibuvStreamWrap::AddMethods(Environment* env,
                                 v8::Local<v8::FunctionTemplate> target,
                                 int flags) {
  // 申请一个函数模板，执行get_write_queue_size的时候会执行GetWriteQueueSize
  Local<FunctionTemplate> get_write_queue_size =
      FunctionTemplate::New(env->isolate(),
                            GetWriteQueueSize,
                            env->as_external(),
                            Signature::New(env->isolate(), target));
  // 在原型上设置方法
  target->PrototypeTemplate()->SetAccessorProperty(
      env->write_queue_size_string(),
      get_write_queue_size,
      Local<FunctionTemplate>(),
      static_cast<PropertyAttribute>(ReadOnly | DontDelete));
  env->SetProtoMethod(target, "setBlocking", SetBlocking);
  StreamBase::AddMethods<LibuvStreamWrap>(env, target, flags);
}

// 获取流对应的文件描述符
int LibuvStreamWrap::GetFD() {
  int fd = -1;
#if !defined(_WIN32)
  if (stream() != nullptr)
    uv_fileno(reinterpret_cast<uv_handle_t*>(stream()), &fd);
#endif
  return fd;
}


bool LibuvStreamWrap::IsAlive() {
  return HandleWrap::IsAlive(this);
}


bool LibuvStreamWrap::IsClosing() {
  return uv_is_closing(reinterpret_cast<uv_handle_t*>(stream()));
}


AsyncWrap* LibuvStreamWrap::GetAsyncWrap() {
  return static_cast<AsyncWrap*>(this);
}


bool LibuvStreamWrap::IsIPCPipe() {
  return is_named_pipe_ipc();
}

// 注册读事件
int LibuvStreamWrap::ReadStart() {
  return uv_read_start(stream(), [](uv_handle_t* handle,
                                    size_t suggested_size,
                                    uv_buf_t* buf) {
    // 分配存储数据的内存
    static_cast<LibuvStreamWrap*>(handle->data)->OnUvAlloc(suggested_size, buf);
  }, [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    // 读取数据成功的回调
    static_cast<LibuvStreamWrap*>(stream->data)->OnUvRead(nread, buf);
  });
}

// 解除可读事件
int LibuvStreamWrap::ReadStop() {
  return uv_read_stop(stream());
}

// 分配内存存储数据
void LibuvStreamWrap::OnUvAlloc(size_t suggested_size, uv_buf_t* buf) {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());

  *buf = EmitAlloc(suggested_size);
}



template <class WrapType, class UVType>
static Local<Object> AcceptHandle(Environment* env, LibuvStreamWrap* parent) {
  EscapableHandleScope scope(env->isolate());
  Local<Object> wrap_obj;
  UVType* handle;
  // 新建一个c++对象。该c++对象关联了一个WrapType对象
  wrap_obj = WrapType::Instantiate(env, parent, WrapType::SOCKET);
  if (wrap_obj.IsEmpty())
    return Local<Object>();

  WrapType* wrap;
  // 把WrapType对象解包出来，存到wrap
  ASSIGN_OR_RETURN_UNWRAP(&wrap, wrap_obj, Local<Object>());
  // 拿到WrapType对象的handle字段
  handle = wrap->UVHandle();
  // 把通信fd存到handle中
  if (uv_accept(parent->stream(), reinterpret_cast<uv_stream_t*>(handle)))
    ABORT();

  return scope.Escape(wrap_obj);
}


void LibuvStreamWrap::OnUvRead(ssize_t nread, const uv_buf_t* buf) {
  HandleScope scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  uv_handle_type type = UV_UNKNOWN_HANDLE;
  // 是unix域，并且作为ipc使用，传递的文件描述符大于0，说明不仅有数据，还有传递过来文件描述符
  if (is_named_pipe_ipc() &&
      uv_pipe_pending_count(reinterpret_cast<uv_pipe_t*>(stream())) > 0) {
    // 当前待读取的fd的类型
    type = uv_pipe_pending_type(reinterpret_cast<uv_pipe_t*>(stream()));
  }

  // We should not be getting this callback if someone as already called
  // uv_close() on the handle.
  CHECK_EQ(persistent().IsEmpty(), false);
  // 成功读取
  if (nread > 0) {
    if (is_tcp()) {
      NODE_COUNT_NET_BYTES_RECV(nread);
    } else if (is_named_pipe()) {
      NODE_COUNT_PIPE_BYTES_RECV(nread);
    }

    Local<Object> pending_obj;
    // 传递的描述符的类型
    if (type == UV_TCP) {
      pending_obj = AcceptHandle<TCPWrap, uv_tcp_t>(env(), this);
    } else if (type == UV_NAMED_PIPE) {
      pending_obj = AcceptHandle<PipeWrap, uv_pipe_t>(env(), this);
    } else if (type == UV_UDP) {
      pending_obj = AcceptHandle<UDPWrap, uv_udp_t>(env(), this);
    } else {
      CHECK_EQ(type, UV_UNKNOWN_HANDLE);
    }

    if (!pending_obj.IsEmpty()) {
      object()->Set(env()->context(),
                    env()->pending_handle_string(),
                    pending_obj).FromJust();
    }
  }

  EmitRead(nread, *buf);
}

// 获取写队列的字节数
void LibuvStreamWrap::GetWriteQueueSize(
    const FunctionCallbackInfo<Value>& info) {
  LibuvStreamWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, info.This());

  if (wrap->stream() == nullptr) {
    info.GetReturnValue().Set(0);
    return;
  }

  uint32_t write_queue_size = wrap->stream()->write_queue_size;
  info.GetReturnValue().Set(write_queue_size);
}

// 设置非阻塞模式
void LibuvStreamWrap::SetBlocking(const FunctionCallbackInfo<Value>& args) {
  LibuvStreamWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  CHECK_GT(args.Length(), 0);
  if (!wrap->IsAlive())
    return args.GetReturnValue().Set(UV_EINVAL);

  bool enable = args[0]->IsTrue();
  args.GetReturnValue().Set(uv_stream_set_blocking(wrap->stream(), enable));
}

// 关闭写端
int LibuvStreamWrap::DoShutdown(ShutdownWrap* req_wrap) {
  int err;
  err = uv_shutdown(req_wrap->req(), stream(), AfterUvShutdown);
  req_wrap->Dispatched();
  return err;
}

// 关闭写端成功后回调
void LibuvStreamWrap::AfterUvShutdown(uv_shutdown_t* req, int status) {
  ShutdownWrap* req_wrap = ShutdownWrap::from_req(req);
  CHECK_NE(req_wrap, nullptr);
  HandleScope scope(req_wrap->env()->isolate());
  Context::Scope context_scope(req_wrap->env()->context());
  req_wrap->Done(status);
}


// NOTE: Call to this function could change both `buf`'s and `count`'s
// values, shifting their base and decrementing their length. This is
// required in order to skip the data that was successfully written via
// uv_try_write().
// 写数据
int LibuvStreamWrap::DoTryWrite(uv_buf_t** bufs, size_t* count) {
  int err;
  size_t written;
  uv_buf_t* vbufs = *bufs;
  size_t vcount = *count;

  err = uv_try_write(stream(), vbufs, vcount);
  if (err == UV_ENOSYS || err == UV_EAGAIN)
    return 0;
  if (err < 0)
    return err;

  // Slice off the buffers: skip all written buffers and slice the one that
  // was partially written.
  written = err;
  for (; vcount > 0; vbufs++, vcount--) {
    // Slice
    if (vbufs[0].len > written) {
      vbufs[0].base += written;
      vbufs[0].len -= written;
      written = 0;
      break;

    // Discard
    } else {
      written -= vbufs[0].len;
    }
  }

  *bufs = vbufs;
  *count = vcount;

  return 0;
}

// 写数据
int LibuvStreamWrap::DoWrite(WriteWrap* w,
                        uv_buf_t* bufs,
                        size_t count,
                        uv_stream_t* send_handle) {
  int r;
  // 需要传递描述符
  if (send_handle == nullptr) {
    r = uv_write(w->req(), stream(), bufs, count, AfterUvWrite);
  } else {
    r = uv_write2(w->req(), stream(), bufs, count, send_handle, AfterUvWrite);
  }

  if (!r) {
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++)
      bytes += bufs[i].len;
    if (stream()->type == UV_TCP) {
      NODE_COUNT_NET_BYTES_SENT(bytes);
    } else if (stream()->type == UV_NAMED_PIPE) {
      NODE_COUNT_PIPE_BYTES_SENT(bytes);
    }
  }

  w->Dispatched();

  return r;
}



void LibuvStreamWrap::AfterUvWrite(uv_write_t* req, int status) {
  WriteWrap* req_wrap = WriteWrap::from_req(req);
  CHECK_NE(req_wrap, nullptr);
  HandleScope scope(req_wrap->env()->isolate());
  Context::Scope context_scope(req_wrap->env()->context());
  req_wrap->Done(status);
}

}  // namespace node

NODE_BUILTIN_MODULE_CONTEXT_AWARE(stream_wrap,
                                  node::LibuvStreamWrap::Initialize)
