diff --git a/Python/ceval.c b/Python/ceval.c
index beabfeb..c963772 100644
--- a/Python/ceval.c
+++ b/Python/ceval.c
@@ -770,6 +770,55 @@ static int unpack_iterable(PyObject *, int, int, PyObject **);
 static int _Py_TracingPossible = 0;
 
 
+#ifdef WITH_THREAD
+#define PULSE_GIL(tstate)                                       \
+    if (_Py_atomic_load_relaxed(&gil_drop_request)) {           \
+        /* Give another thread a chance */                      \
+        if (PyThreadState_Swap(NULL) != tstate)                 \
+            Py_FatalError("ceval: tstate mix-up");              \
+        drop_gil(tstate);                                       \
+                                                                \
+        /* Other threads may run now */                         \
+                                                                \
+        take_gil(tstate);                                       \
+                                                                \
+        /* Check if we should make a quick exit. */             \
+        if (_Py_Finalizing && _Py_Finalizing != tstate) {       \
+            drop_gil(tstate);                                   \
+            PyThread_exit_thread();                             \
+        }                                                       \
+                                                                \
+        if (PyThreadState_Swap(tstate) != NULL)                 \
+            Py_FatalError("ceval: orphan tstate");              \
+    }
+
+#endif
+
+int _PyEval_PeriodicWork(void) {
+    if (_Py_atomic_load_relaxed(&eval_breaker)) {
+        if (_Py_atomic_load_relaxed(&pendingcalls_to_do)) {
+            if (Py_MakePendingCalls() < 0) {
+                return 1;
+            }
+        }
+
+        PyThreadState* tstate = PyThreadState_GET();
+#ifdef WITH_THREAD
+        PULSE_GIL(tstate);
+#endif
+
+        if (tstate->async_exc != NULL) {
+            PyObject *exc = tstate->async_exc;
+            tstate->async_exc = NULL;
+            UNSIGNAL_ASYNC_EXC();
+            PyErr_SetNone(exc);
+            Py_DECREF(exc);
+            return 1;
+        }
+    }
+    return 0;
+}
+
 
 PyObject *
 PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
@@ -796,6 +845,31 @@ PyEval_EvalFrame(PyFrameObject *f) {
 PyObject *
 PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
 {
+    if (f->f_code->co_jitted != NULL) {
+        return f->f_code->co_jitted->j_evalfunc(f->f_code->co_jitted->j_evalstate, f);
+    }
+
+    if (!f->f_code->co_compilefailed /*&& f->f_code->co_runcount++ >20 */) {
+        PyThreadState *tstate = PyThreadState_GET();
+        if (tstate->interp->jitcompile != NULL) {
+            f->f_code->co_jitted = tstate->interp->jitcompile((PyObject*)f->f_code);
+            if (f->f_code->co_jitted != NULL) {
+                // execute the jitted code...
+                return f->f_code->co_jitted->j_evalfunc(f->f_code->co_jitted->j_evalstate, f);
+            }
+
+            // no longer try and compile this method...
+            f->f_code->co_compilefailed = 1;
+        }
+    }
+
+    return PyEval_EvalFrameEx_NoJit(f, throwflag);
+}
+
+
+PyObject *
+PyEval_EvalFrameEx_NoJit(PyFrameObject *f, int throwflag)
+{
 #ifdef DXPAIRS
     int lastopcode = 0;
 #endif
@@ -1262,25 +1336,7 @@ PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
                     goto error;
             }
 #ifdef WITH_THREAD
-            if (_Py_atomic_load_relaxed(&gil_drop_request)) {
-                /* Give another thread a chance */
-                if (PyThreadState_Swap(NULL) != tstate)
-                    Py_FatalError("ceval: tstate mix-up");
-                drop_gil(tstate);
-
-                /* Other threads may run now */
-
-                take_gil(tstate);
-
-                /* Check if we should make a quick exit. */
-                if (_Py_Finalizing && _Py_Finalizing != tstate) {
-                    drop_gil(tstate);
-                    PyThread_exit_thread();
-                }
-
-                if (PyThreadState_Swap(tstate) != NULL)
-                    Py_FatalError("ceval: orphan tstate");
-            }
+            PULSE_GIL(tstate);
 #endif
             /* Check for asynchronous exceptions. */
             if (tstate->async_exc != NULL) {