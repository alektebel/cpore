/* Python's headers first, and before the forward declarations below rather
 * than only by way of env_binding.h - `PyObject` has to be a type before a
 * prototype can name one. */
#include <Python.h>
#include <numpy/arrayobject.h>

#include "cell.h"

#define Env CellEnv

/* One extra method beyond the standard set: a frame.
 *
 * PufferLib's c_render owns a window; cpore's renderer owns a buffer. Handing
 * the buffer up to Python instead means the same pixels can go to a window, a
 * PNG, a video or a browser without the environment knowing which, and it
 * keeps the simulation free of a windowing dependency. */
static PyObject *cell_render_rgba(PyObject *self, PyObject *args);
static PyObject *cell_dims(PyObject *self, PyObject *args);
static PyObject *cell_greedy(PyObject *self, PyObject *args);

/* Two entries, not one: the macro is expanded inside the method table's
 * initialiser list, so a comma in it is just another element. */
#define MY_METHODS \
    {"render_rgba", cell_render_rgba, METH_VARARGS, \
     "Draw one env into a contiguous uint8 RGBA array"}, \
    {"dims", cell_dims, METH_NOARGS, \
     "Sizes the Python side would otherwise have to hardcode"}, \
    {"greedy", cell_greedy, METH_VARARGS, \
     "Fill an action array from the scripted baseline"}

/* setup.py puts pufferlib/ocean on the include path, so this is the installed
 * PufferLib's own binding layer rather than a vendored copy that would drift
 * out of step with it. */
#include "env_binding.h"

/* An optional keyword, with a default. `unpack` raises when a key is absent,
 * which is right for a required parameter and wrong for a tuning knob. */
static double unpack_or(PyObject *kwargs, const char *key, double fallback)
{
    if (!kwargs) return fallback;
    PyObject *v = PyDict_GetItemString(kwargs, key);
    if (!v || v == Py_None) return fallback;
    if (PyLong_Check(v))  return (double)PyLong_AsLong(v);
    if (PyFloat_Check(v)) return PyFloat_AsDouble(v);
    if (PyBool_Check(v))  return (v == Py_True) ? 1.0 : 0.0;
    return fallback;
}

static int my_init(Env *env, PyObject *args, PyObject *kwargs)
{
    (void)args;
    env->seed        = (uint32_t)unpack_or(kwargs, "seed", 0);
    env->episode_len = (int)unpack_or(kwargs, "episode_len", CP_MAX_STEPS);
    env->respawn     = (int)unpack_or(kwargs, "respawn", 1);
    if (env->episode_len < 16) env->episode_len = 16;
    c_reset(env);
    return 0;
}

static int my_log(PyObject *dict, Log *log)
{
    assign_to_dict(dict, "score",      log->score);
    assign_to_dict(dict, "dna",        log->dna);
    assign_to_dict(dict, "tier",       log->tier);
    assign_to_dict(dict, "generation", log->generation);
    assign_to_dict(dict, "plants",     log->plants);
    assign_to_dict(dict, "meat",       log->meat);
    assign_to_dict(dict, "kills",      log->kills);
    assign_to_dict(dict, "gulps",      log->gulps);
    assign_to_dict(dict, "gulped",     log->gulped);
    assign_to_dict(dict, "deaths",     log->deaths);
    assign_to_dict(dict, "evolved",    log->evolved);
    assign_to_dict(dict, "length",     log->length);
    return 0;
}

static PyObject *cell_dims(PyObject *self, PyObject *args)
{
    (void)self; (void)args;
    PyObject *d = PyDict_New();
    if (!d) return NULL;
    struct { const char *k; long v; } items[] = {
        { "obs_dim",    CP_OBS_DIM },
        { "n_move",     9 },
        { "max_parts",  CP_MAX_PARTS },
        { "part_count", CP_PART_COUNT },
        { "max_steps",  CP_MAX_STEPS },
        { "tiers",      CP_TIERS },
        { "generations", CP_GENERATIONS },
        { "world_w",    (long)CP_WORLD_W },
        { "world_h",    (long)CP_WORLD_H },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        PyObject *v = PyLong_FromLong(items[i].v);
        if (!v || PyDict_SetItemString(d, items[i].k, v) < 0) {
            Py_XDECREF(v); Py_DECREF(d); return NULL;
        }
        Py_DECREF(v);
    }
    return d;
}

/* greedy(vec_handle, int32 (num_envs, 3) array)
 *
 * The scripted baseline, projected onto the discrete action space so it plays
 * the same game a policy does. It exists to answer the question a new
 * environment always has to answer before any training result means anything:
 * is a decent score reachable at all, and how far above random is it? A
 * baseline that cheats by using a wider action space than the learner cannot
 * answer that, so this one does not. */
static PyObject *cell_greedy(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle, *arr;
    if (!PyArg_ParseTuple(args, "OO", &handle, &arr)) return NULL;
    if (!PyObject_TypeCheck(handle, &PyLong_Type)) {
        PyErr_SetString(PyExc_TypeError, "first argument must be a vec handle");
        return NULL;
    }
    VecEnv *vec = (VecEnv *)PyLong_AsVoidPtr(handle);
    if (!vec) {
        PyErr_SetString(PyExc_ValueError, "invalid vec handle");
        return NULL;
    }
    if (!PyObject_TypeCheck(arr, &PyArray_Type)) {
        PyErr_SetString(PyExc_TypeError, "actions must be a NumPy array");
        return NULL;
    }
    PyArrayObject *a = (PyArrayObject *)arr;
    if (!PyArray_ISCONTIGUOUS(a) || PyArray_TYPE(a) != NPY_INT32
        || PyArray_NDIM(a) != 2 || PyArray_DIM(a, 1) != 3
        || PyArray_DIM(a, 0) != vec->num_envs) {
        PyErr_SetString(PyExc_ValueError,
                        "actions must be a contiguous int32 (num_envs, 3) array");
        return NULL;
    }

    int32_t *out = (int32_t *)PyArray_DATA(a);
    for (int i = 0; i < vec->num_envs; i++) {
        float act[CP_ACT_DIM];
        cp_policy_greedy(&vec->envs[i]->world, act);

        /* Nearest compass point, by dot product. Below a threshold the
         * baseline is not really steering anywhere, and drifting is a legal
         * answer, so say so rather than rounding noise into a direction. */
        int best = 0;
        float bd = 0.30f;
        for (int k = 1; k < 9; k++) {
            float d = act[0] * MOVE_X[k] + act[1] * MOVE_Y[k];
            if (d > bd) { bd = d; best = k; }
        }
        out[i * 3 + 0] = best;
        out[i * 3 + 1] = act[2] > 0.5f;
        out[i * 3 + 2] = act[3] > 0.5f;
    }
    Py_RETURN_NONE;
}

/* render_rgba(vec_handle, env_id, array, style) */
static PyObject *cell_render_rgba(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle, *arr;
    int env_id, style;
    if (!PyArg_ParseTuple(args, "OiOi", &handle, &env_id, &arr, &style))
        return NULL;
    if (!PyObject_TypeCheck(handle, &PyLong_Type)) {
        PyErr_SetString(PyExc_TypeError, "first argument must be a vec handle");
        return NULL;
    }
    VecEnv *vec = (VecEnv *)PyLong_AsVoidPtr(handle);
    if (!vec || env_id < 0 || env_id >= vec->num_envs) {
        PyErr_SetString(PyExc_ValueError, "env_id out of range");
        return NULL;
    }
    if (!PyObject_TypeCheck(arr, &PyArray_Type)) {
        PyErr_SetString(PyExc_TypeError, "frame must be a NumPy array");
        return NULL;
    }
    PyArrayObject *fb = (PyArrayObject *)arr;
    if (!PyArray_ISCONTIGUOUS(fb) || PyArray_TYPE(fb) != NPY_UINT8
        || PyArray_NDIM(fb) != 3 || PyArray_DIM(fb, 2) != 4) {
        PyErr_SetString(PyExc_ValueError, "frame must be a contiguous uint8 (H, W, 4) array");
        return NULL;
    }
    int h = (int)PyArray_DIM(fb, 0), w = (int)PyArray_DIM(fb, 1);
    cp_render_styled(&vec->envs[env_id]->world, (unsigned char *)PyArray_DATA(fb),
                     w, h, style);
    Py_RETURN_NONE;
}
