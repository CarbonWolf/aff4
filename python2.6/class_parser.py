import sys, os, re, pdb

DEBUG = 0

def log(msg):
    if DEBUG>0:
        sys.stderr.write(msg+"\n")

def escape_for_string(string):
    result = string
    result = result.encode("string-escape")
    result = result.replace('"',r'\"')

    return result

class Module:
    def __init__(self, name):
        self.name = name
        self.constants = []
        self.classes = {}
        self.headers = "#include <Python.h>\n"

    def initialization(self):
        result = """
//talloc_enable_leak_report_full();
AFF4_Init();

"""
        for cls in self.classes.values():
            if cls.is_active():
                result += cls.initialise()

        return result

    def add_constant(self, constant, type="numeric"):
        """ This will be called to add #define constant macros """
        self.constants.append((constant, type))

    def add_class(self, cls, handler):
        self.classes[cls.class_name] = cls

        ## Make a wrapper in the type dispatcher so we can handle
        ## passing this class from/to python
        type_dispatcher[cls.class_name] = handler

    def private_functions(self):
        """ Emits hard coded private functions for doing various things """
        return """
/* The following is a static array mapping CLASS() pointers to their
python wrappers. This is used to allow the correct wrapper to be
chosen depending on the object type found - regardless of the
prototype.

This is basically a safer way for us to cast the correct python type
depending on context rather than assuming a type based on the .h
definition. For example consider the function

AFFObject Resolver.open(uri, mode)

The .h file implies that an AFFObject object is returned, but this is
not true as most of the time an object of a derived class will be
returned. In C we cast the returned value to the correct type. In the
python wrapper we just instantiate the correct python object wrapper
at runtime depending on the actual returned type. We use this lookup
table to do so.
*/
static int TOTAL_CLASSES=0;

static struct python_wrapper_map_t {
       Object class_ref;
       PyTypeObject *python_type;
} python_wrappers[%s];

/** This is a generic wrapper type */
typedef struct {
  PyObject_HEAD
  void *base;
} Gen_wrapper;

/* Create the relevant wrapper from the item based on the lookup
table.
*/
Gen_wrapper *new_class_wrapper(Object item) {
   int i;
   Gen_wrapper *result;

   for(i=0; i<TOTAL_CLASSES; i++) {
     if(python_wrappers[i].class_ref == item->__class__) {
       result = (Gen_wrapper *)_PyObject_New(python_wrappers[i].python_type);
       result->base = (void *)item;

       return result;
     };
   };

  PyErr_Format(PyExc_RuntimeError, "Unable to find a wrapper for object %%s", NAMEOF(item));
  return NULL;
};

static int type_check(PyObject *obj, PyTypeObject *type) {
   PyTypeObject *tmp;

   // Recurse through the inheritance tree and check if the types are expected
   for(tmp = obj->ob_type; tmp != &PyBaseObject_Type; tmp = tmp->tp_base) {
     if(tmp == type) return 1;
   };

  return 0;
};

""" % (len(self.classes)+1)

    def initialise_class(self, class_name, out, done = None):
        if done and class_name in done: return

        done.add(class_name)

        cls = self.classes[class_name]
        """ Write out class initialisation code into the main init function. """
        if cls.is_active():
            base_class = self.classes.get(cls.base_class_name)

            if base_class and base_class.is_active():
                ## We have a base class - ensure it gets written out
                ## first:
                self.initialise_class(cls.base_class_name, out, done)

                ## Now assign ourselves as derived from them
                out.write(" %s_Type.tp_base = &%s_Type;" % (
                        cls.class_name, cls.base_class_name))

            out.write("""
 %(name)s_Type.tp_new = PyType_GenericNew;
 if (PyType_Ready(&%(name)s_Type) < 0)
     return;

 Py_INCREF((PyObject *)&%(name)s_Type);
 PyModule_AddObject(m, "%(name)s", (PyObject *)&%(name)s_Type);
""" % {'name': cls.class_name})

    def write(self, out):
        out.write(self.headers)
        out.write(self.private_functions())

        for cls in self.classes.values():
            if cls.is_active():
                cls.struct(out)
                cls.prototypes(out)

        out.write("/*****************************************************\n             Implementation\n******************************************************/\n\n")
        for cls in self.classes.values():
            if cls.is_active():
                cls.PyMethodDef(out)
                cls.code(out)
                cls.PyTypeObject(out)

        ## Write the module initializer
        out.write("""
static PyMethodDef %(module)s_methods[] = {
     {NULL}  /* Sentinel */
};

PyMODINIT_FUNC init%(module)s(void) {
   /* create module */
   PyObject *m = Py_InitModule3("%(module)s", %(module)s_methods,
                                   "%(module)s module.");
   PyObject *d = PyModule_GetDict(m);
   PyObject *tmp;
""" % {'module': self.name})

        ## The trick is to initialise the classes in order of their
        ## inheritance. The following code will order initializations
        ## according to their inheritance tree
        done = set()
        for class_name in self.classes.keys():
            self.initialise_class(class_name, out, done)

        ## Add the constants in here
        for constant, type in self.constants:
            if type == 'numeric':
                out.write(""" tmp = PyLong_FromUnsignedLongLong(%s); \n""" % constant)
            elif type == 'string':
                out.write(" tmp = PyString_FromString(%s); \n" % constant)

            out.write("""
 PyDict_SetItemString(d, "%s", tmp);
 Py_DECREF(tmp);\n""" % (constant))

        out.write(self.initialization())
        out.write("}\n\n")

class Type:
    interface = None
    buidstr = 'O'
    sense = 'IN'

    def __init__(self, name, type):
        self.name = name
        self.type = type
        self.attributes = set()

    def comment(self):
        return "%s %s " % (self.type, self.name)

    def python_name(self):
        return self.name

    def definition(self, default=None):
        if default:
            return "%s %s=%s;\n" % (self.type, self.name, default)
        else:
            return "%s %s;\n" % (self.type, self.name)

    def byref(self):
        return "&%s" % self.name

    def call_arg(self):
        return self.name

    def pre_call(self, method):
        return ''

    def assign(self, call, method, target=None):
        return "%s = %s;\n" % (target or self.name, call)

    def post_call(self, method):
        if "DESTRUCTOR" in self.attributes:
            return "self->base = NULL;\n"

        return ''

class String(Type):
    interface = 'string'
    buidstr = 's'

    def __init__(self, name, type):
        Type.__init__(self, name, type)
        self.length = "strlen(%s)" % name

    def byref(self):
        return "&%s" % self.name

    def to_python_object(self, name=None, **kw):
        name = name or self.name

        result = "py_result = PyString_FromStringAndSize((char *)%s, %s);\n" % (name, self.length)
        if "BORROWED" not in self.attributes:
            result += "talloc_free(%s);\n" % name

        return result

class BorrowedString(String):
    def to_python_object(self, name=None, **kw):
        name = name or self.name
        return "py_result = PyString_FromStringAndSize((char *)%(name)s, %(length)s);\n" % dict(name=name, length=self.length)

class Char_and_Length(Type):
    interface = 'char_and_length'
    buidstr = 's#'

    def __init__(self, data, data_type, length, length_type):
        Type.__init__(self, data, data_type)

        self.name = data
        self.data_type=data_type
        self.length = length
        self.length_type = length_type

    def comment(self):
        return "%s %s, %s %s" % (self.data_type, self.name,
                                 self.length_type, self.length)

    def definition(self, default = '""'):
        return "char *%s=%s; Py_ssize_t %s=strlen(%s);\n" % (
            self.name, default,
            self.length, default)

    def byref(self):
        return "&%s, &%s" % (self.name, self.length)

    def call_arg(self):
        return "(%s)%s, (%s)%s" % (self.data_type, self.name, self.length_type,
                                   self.length)

class Integer(Type):
    interface = 'integer'
    buidstr = 'K'

    def __init__(self, name,type):
        Type.__init__(self,name,type)
        self.type = 'uint64_t '

    def definition(self, default = 0):
        return Type.definition(self, default)

    def to_python_object(self, name=None, *kw):
        name = name or self.name
        return "py_result = PyLong_FromLong(%s);\n" % name

class Char(Integer):
    buidstr = "s"
    interface = 'small_integer'

    def to_python_object(self, name = None, **kw):
        ## We really want to return a string here
        return """str_%(name)s = &%(name)s;
    py_result = PyString_FromStringAndSize(str_%(name)s, 1);
""" % dict(name = name or self.name)

    def definition(self, default = '"\\x0"'):
        return "char %s=0; char *str_%s = %s;\n" % (
            self.name,self.name, default)

    def byref(self):
        return "&str_%s" % self.name

    def pre_call(self, method):
        method.error_set = True
        return """
if(strlen(str_%(name)s)!=1) {
  PyErr_Format(PyExc_RuntimeError,
          "You must only provide a single character for arg %(name)r");
  goto error;
};

%(name)s = str_%(name)s[0];
""" % dict(name = self.name)

class StringOut(String):
    sense = 'OUT'

class Char_and_Length_OUT(Char_and_Length):
    sense = 'OUT_DONE'
    buidstr = 'l'

    def definition(self, default = None):
        return Char_and_Length.definition(self) + "PyObject *tmp_%s;\n" % self.name

    def python_name(self):
        return self.length

    def byref(self):
        return "&%s" % self.length

    def pre_call(self, method):
        return """tmp_%s = PyString_FromStringAndSize(NULL, %s);
PyString_AsStringAndSize(tmp_%s, &%s, (Py_ssize_t *)&%s);
""" % (self.name, self.length, self.name, self.name, self.length)

    def to_python_object(self, name=None, **kw):
        name = name or self.name

        if 'results' in kw:
            kw['results'].pop(0)

        return """ _PyString_Resize(&tmp_%s, func_return); \npy_result = tmp_%s;\n""" % (
            name, name)

class TDB_DATA_P(Char_and_Length_OUT):
    def __init__(self, name, type):
        Type.__init__(self, name, type)

    def definition(self, default=None):
        return Type.definition(self)

    def byref(self):
        return "%s.dptr, &%s.dsize" % (self.name, self.name)

    def pre_call(self, method):
        return ''

    def call_arg(self):
        return Type.call_arg(self)

    def to_python_object(self, name=None, **kw):
        name = name or self.name
        return "py_result = PyString_FromStringAndSize((char *)%s->dptr, %s->dsize);"\
            "\ntalloc_free(%s);" % (
            name, name, name)

class TDB_DATA(TDB_DATA_P):
    def to_python_object(self, name = None, **kw):
        name = name or self.name

        return "py_result = PyString_FromStringAndSize((char *)%s.dptr, %s.dsize);\n" % (
            name, name)

class Void(Type):
    buidstr = ''

    def __init__(self, *args):
        Type.__init__(self, None, 'void *')

    def definition(self, default = None):
        return ''

    def to_python_object(self, name=None, **kw):
        return "Py_INCREF(Py_None); py_result = Py_None;\n"

    def call_arg(self):
        return "NULL"

    def byref(self):
        return None

    def assign(self, call, method, target=None):
        ## We dont assign the result to anything
        return "%s;\n" % call

class Wrapper(Type):
    """ This class represents a wrapped C type """
    sense = 'IN'

    def to_python_object(self, **kw):
        return ''

    def definition(self, default = None):
        return "Gen_wrapper *%s;\n" % (self.name)

    def call_arg(self):
        return "%s->base" % self.name

    def pre_call(self, method):
        if 'OUT' in self.attributes or self.sense == 'OUT':
            return ''

        return """if(!type_check((PyObject *)%s,&%s_Type)) {
     PyErr_Format(PyExc_RuntimeError, "%s must be derived from type %s");
     goto error;
};\n""" % (self.name, self.type, self.name, self.type)

    def assign(self, call, method, target=None):
        method.error_set = True;
        args = dict(name = target or self.name, call = call, type = self.type)

        result = """{
       Object returned_object = (Object)%(call)s;

       if(!returned_object) {
         PyErr_Format(PyExc_RuntimeError,
                    "Failed to create object %(type)s: %%s", __error_str);
         ClearError();
         goto error;
       };

       %(name)s = new_class_wrapper(returned_object);
       if(!%(name)s) goto error;
    }
""" % args

        if "BORROWED" in self.attributes:
            result += "talloc_increase_ref_count(%(name)s->base);\n" % args

        return result

    def to_python_object(self, name=None, **kw):
        name = name or self.name
        return "py_result = (PyObject *)%(name)s;\n" % dict(name = name)


class StructWrapper(Wrapper):
    """ A wrapper for struct classes """
    def assign(self, call, method, target = None):
        args = dict(name = target or self.name, call = call, type = self.type)
        result = """
%(name)s = (py%(type)s *)PyObject_New(py%(type)s, &%(type)s_Type);
%(name)s->base = %(call)s;
""" % args

        if "BORROWED" in self.attributes:
            result += "talloc_increase_ref_count(%(name)s->base);\n" % args

        return result

    def byref(self):
        return "&%s" % self.name

class PointerStructWrapper(StructWrapper):
    def __init__(self, name, type):
        type = type.split()[0]
        Wrapper.__init__(self,name, type)

class Timeval(Type):
    """ handle struct timeval values """
    interface = 'numeric'
    buidstr = 'f'

    def definition(self, default = None):
        return "float %(name)s_flt; struct timeval %(name)s;\n" % self.__dict__

    def byref(self):
        return "&%s_flt" % self.name

    def pre_call(self, method):
        return "%(name)s.tv_sec = (int)%(name)s_flt; %(name)s.tv_usec = (%(name)s_flt - %(name)s.tv_sec) * 1e6;\n" % self.__dict__

    def to_python_object(self, name=None, **kw):
        name = name or self.name
        return """%(name)s_flt = (double)(%(name)s.tv_sec) + %(name)s.tv_usec;
py_result = PyFloat_FromDouble(%(name)s_flt);
""" % dict(name = name)

type_dispatcher = {
    "IN char *": String,
    "IN unsigned char *": String,
    "unsigned char *": String,
    "char *": String,

    "OUT char *": StringOut,
    "OUT unsigned char *": StringOut,
    "unsigned int": Integer,
    'int': Integer,
    'char': Char,
    'void': Void,
    'void *': Void,

    'TDB_DATA *': TDB_DATA_P,
    'TDB_DATA': TDB_DATA,
    'uint64_t': Integer,
    'uint32_t': Integer,
    'uint16_t': Integer,
    'int64_t': Integer,
    'unsigned long int': Integer,
    'struct timeval': Timeval,
    }

method_attributes = ['BORROWED', 'DESTRUCTOR']

def dispatch(name, type):
    type_components = type.split()
    attributes = set()

    if type_components[0] in method_attributes:
        attributes.add(type_components.pop(0))

    type = " ".join(type_components)
    result = type_dispatcher[type](name, type)
    result.attributes = attributes

    return result


class ResultException:
    value = 0
    exception = "PyExc_IOError"

    def __init__(self, check, exception, message):
        self.check = check
        self.exception = exception
        self.message = message

    def write(self, out):
        out.write("\n//Handle exceptions\n")
        out.write("if(%s) {\n    PyErr_Format(PyExc_%s, %s);\n  goto error; \n};\n\n" % (
                self.check, self.exception, self.message))

class Method:
    default_re = re.compile("DEFAULT\(([A-Za-z0-9]+)\) =(.+)")
    exception_re = re.compile("RAISES\(([^,]+),([^\)]+)\) =(.+)")

    def __init__(self, class_name, base_class_name, method_name, args, return_type,
                 myclass = None):
        self.name = method_name
        ## myclass needs to be a class generator
        if not isinstance(myclass, ClassGenerator): raise RuntimeError("myclass must be a class generator")

        self.myclass = myclass
        self.docstring = ''
        self.defaults = {}
        self.exception = None
        self.error_set = False
        self.class_name = class_name
        self.base_class_name = base_class_name
        self.args = []
        self.definition_class_name = class_name
        for type,name in args:
            self.add_arg(type, name)

        try:
            self.return_type = dispatch('func_return', return_type)
            self.return_type.attributes.add("OUT")
        except KeyError:
            ## Is it a wrapped type?
            log("Unable to handle return type %s.%s %s" % (self.class_name, self.name, return_type))
            pdb.set_trace()
            self.return_type = Void()

    def clone(self, new_class_name):
        result = self.__class__(new_class_name, self.base_class_name, self.name,
                                [], 'void *',
                                myclass = self.myclass)
        result.args = self.args
        result.return_type = self.return_type
        result.definition_class_name = self.definition_class_name

        return result

    def find_optional_vars(self):
        for line in self.docstring.splitlines():
            m =self.default_re.search(line)
            if m:
                name = m.group(1)
                value = m.group(2)
                log("Setting default value for %s of %s" % (m.group(1),
                                                            m.group(2)))
                self.defaults[name] = value

            m =self.exception_re.search(line)
            if m:
                self.exception = ResultException(m.group(1), m.group(2), m.group(3))

    def write_local_vars(self, out):
        self.find_optional_vars()

        ## We do it in two passes - first mandatory then optional
        kwlist = """static char *kwlist[] = {"""
        ## Mandatory
        for type in self.args:
            python_name = type.python_name()
            if python_name and type.name not in self.defaults:
                kwlist += '"%s",' % python_name

        for type in self.args:
            python_name = type.python_name()
            if python_name and type.name in self.defaults:
                kwlist += '"%s",' % python_name

        kwlist += ' NULL};\n'

        for type in self.args:
            try:
                out.write(type.definition(default = self.defaults[type.name]))
            except KeyError:
                out.write(type.definition())

        ## Make up the format string for the parse args in two pases
        parse_line = ''
        for type in self.args:
            if type.buidstr and type.name not in self.defaults:
                parse_line += type.buidstr

        parse_line += '|'
        for type in self.args:
            if type.buidstr and type.name in self.defaults:
                parse_line += type.buidstr

        if parse_line != '|':
            ## Now parse the args from python objects
            out.write(kwlist)
            out.write("\nif(!PyArg_ParseTupleAndKeywords(args, kwds, \"%s\", kwlist, " % parse_line)
            tmp = []
            for type in self.args:
                ref = type.byref()
                if ref:
                    tmp.append(ref)

            out.write(",".join(tmp))
            self.error_set = True
            out.write("))\n goto error;\n\n")

    def error_condition(self):
        result = ""
        if "DESTRUCTOR" in self.return_type.attributes:
            result += "self->base = NULL;\n"

        return result +"return NULL;\n";

    def write_definition(self, out):
        args = dict(method = self.name, class_name = self.class_name)
        out.write("\n/********************************************************\nAutogenerated wrapper for function:\n")
        out.write(self.comment())
        out.write("********************************************************/\n")

        out.write("""
static PyObject *py%(class_name)s_%(method)s(py%(class_name)s *self, PyObject *args, PyObject *kwds) {
       PyObject *returned_result, *py_result;
""" % args)

        out.write(self.return_type.definition())

        self.write_local_vars( out);

        out.write("""// Make sure that we have something valid to wrap
if(!self->base) return PyErr_Format(PyExc_RuntimeError, "%(class_name)s object no longer valid");
""" % args)

        ## Precall preparations
        out.write("// Precall preparations\n")
        out.write(self.return_type.pre_call(self))
        for type in self.args:
            out.write(type.pre_call(self))

        out.write("\n// Make the call\n")
        call = "((%s)self->base)->%s(((%s)self->base)" % (self.definition_class_name, self.name, self.definition_class_name)
        tmp = ''
        for type in self.args:
            tmp += ", " + type.call_arg()

        call += "%s)" % tmp

        ## Now call the wrapped function
        out.write(self.return_type.assign(call, self))
        if self.exception:
            self.exception.write(out)

        out.write("\n// Postcall preparations\n")
        ## Postcall preparations
        out.write(self.return_type.post_call(self))
        for type in self.args:
            out.write(type.post_call(self))

        ## Now assemble the results
        results = [self.return_type.to_python_object()]
        for type in self.args:
            if type.sense == 'OUT_DONE':
                results.append(type.to_python_object(results = results))

        out.write("\n// prepare results\n")
        ## Make a tuple of results and pass them back
        if len(results)>1:
            out.write("returned_result = PyList_New(0);\n")
            for result in results:
                out.write(result)
                out.write("PyList_Append(returned_result, py_result); Py_DECREF(py_result);\n");
            out.write("return returned_result;\n")
        else:
            out.write(results[0])
            ## This useless code removes compiler warnings
            out.write("returned_result = py_result;\nreturn returned_result;\n");

        ## Write the error part of the function
        if self.error_set:
            out.write("\n// error conditions:\n")
            out.write("error:\n    " + self.error_condition());

        out.write("\n};\n\n")

    def add_arg(self, type, name):
        try:
            t = type_dispatcher[type](name, type)
        except KeyError:
            log( "Unable to handle type %s.%s %s" % (self.class_name, self.name, type))
            return

        ## Here we collapse char * + int type interfaces into a
        ## coherent string like interface.
        try:
            previous = self.args[-1]
            if t.interface == 'integer' and \
                    previous.interface == 'string':

                ## We make a distinction between IN variables and OUT
                ## variables
                if previous.sense == 'OUT':
                    cls = Char_and_Length_OUT
                else:
                    cls = Char_and_Length

                self.args[-1] = cls(
                    previous.name,
                    previous.type,
                    name, type)
                return
        except IndexError:
            pass

        self.args.append(t)

    def comment(self):
        result = ''
        #result += " {%s (%s)}" % (self.return_type.__class__.__name__, self.return_type.attributes)
        result += self.return_type.type+" "+self.class_name+"."+self.name+"("
        args = []
        for type in self.args:
            #result += " {%s (%s)} " %( type.__class__.__name__, type.attributes)
            args.append( type.comment())

        result += ",".join(args) + ");\n"

        return result

    def prototype(self, out):
        out.write("""static PyObject *py%(class_name)s_%(method)s(py%(class_name)s *self, PyObject *args, PyObject *kwds);\n""" % dict(method = self.name, class_name = self.class_name))

class ConstructorMethod(Method):
    ## Python constructors are a bit different than regular methods
    def prototype(self, out):
        out.write("""
static int py%(class_name)s_init(py%(class_name)s *self, PyObject *args, PyObject *kwds);
""" % dict(method = self.name, class_name = self.class_name))

    def write_destructor(self, out):
        free = """
    if(self->base) {
        talloc_free(self->base);
        self->base=NULL;
    };
"""
        out.write("""static void
%(class_name)s_dealloc(py%(class_name)s *self) {
%(free)s
};\n
""" % dict(class_name = self.class_name, free=free))

    def error_condition(self):
        return "return -1;";

    def write_definition(self, out):
        out.write("""static int py%(class_name)s_init(py%(class_name)s *self, PyObject *args, PyObject *kwds) {\n""" % dict(method = self.name, class_name = self.class_name))

        self.write_local_vars(out)

        ## Precall preparations
        for type in self.args:
            out.write(type.pre_call(self))

        ## Now call the wrapped function
        out.write("\nself->base = CONSTRUCT(%s, %s, %s, NULL" % (self.class_name,
                                                                 self.definition_class_name,
                                                                 self.name))
        tmp = ''
        for type in self.args:
            tmp += ", " + type.call_arg()

        self.error_set = True
        out.write("""%s);
  if(!self->base) {
    PyErr_Format(PyExc_IOError, "Unable to construct class %s");
    goto error;
  };
""" % (tmp, self.class_name))

        out.write("  return 0;\n");

        ## Write the error part of the function
        if self.error_set:
            out.write("error:\n    " + self.error_condition());

        out.write("\n};\n\n")

class GetattrMethod(Method):
    def __init__(self, class_name, base_class_name):
        self.class_name = class_name
        self.base_class_name = base_class_name
        self.attributes = []
        self.error_set = True
        self.return_type = Void()

    def add_attribute(self, attr):
        if attr.name:
            self.attributes.append((self.class_name, attr))

    def clone(self, class_name):
        result = self.__class__(class_name, self.base_class_name)
        result.attributes = self.attributes[:]

        return result

    def prototype(self, out):
        out.write("""
static PyObject *%(class_name)s_getattr(py%(class_name)s *self, PyObject *name);
""" % self.__dict__)

    def built_ins(self, out):
        """ check for some built in attributes we need to support """
        out.write("""  if(!strcmp(name, "__members__")) {
     PyObject *result = PyList_New(0);
     PyObject *tmp;
     PyMethodDef *i;

     if(!result) goto error;
""")
        ## Add attributes
        for class_name, attr in self.attributes:
            out.write(""" tmp = PyString_FromString("%(name)s");
    PyList_Append(result, tmp); Py_DECREF(tmp);
""" % dict(name = attr.name))

        ## Add methods
        out.write("""

    for(i=%s_methods; i->ml_name; i++) {
     tmp = PyString_FromString(i->ml_name);
    PyList_Append(result, tmp); Py_DECREF(tmp);
    }; """ % self.class_name)

        out.write("""
     return result; 
   }\n""")

    def write_definition(self, out):
        out.write("""
static PyObject *%(class_name)s_getattr(py%(class_name)s *self, PyObject *pyname) {
  char *name = PyString_AsString(pyname);

  if(!self->base) return PyErr_Format(PyExc_RuntimeError, "Wrapped object no longer valid");
  if(!name) return NULL;
""" % self.__dict__)

        self.built_ins(out)

        for class_name, attr in self.attributes:
            ## what we want to assign
            if self.base_class_name:
                call = "(((%s)self->base)->%s)" % (class_name, attr.name)
            else:
                call = "(self->base->%s)" % (attr.name)

            out.write("""
if(!strcmp(name, "%(name)s")) {
    PyObject *py_result;
    %(python_def)s

    %(python_assign)s
    %(python_obj)s
    return py_result;
};""" % dict(name = attr.name, python_obj = attr.to_python_object(),
             python_assign = attr.assign(call, self),
             python_def = attr.definition()))

        out.write("""

  // Hand it off to the python native handler
  return PyObject_GenericGetAttr((PyObject *)self, pyname);
""" % self.__dict__)

        ## Write the error part of the function
        if self.error_set:
            out.write("error:\n" + self.error_condition());

        out.write("}\n\n")

class StructConstructor(ConstructorMethod):
    """ A constructor for struct wrappers - basically just allocate
    memory for the struct.
    """
    def write_definition(self, out):
        out.write("""static int py%(class_name)s_init(py%(class_name)s *self, PyObject *args, PyObject *kwds) {\n""" % dict(method = self.name, class_name = self.class_name))

        out.write("\nself->base = NULL;\n")
        out.write("  return 0;\n};\n\n")

    def write_destructor(self, out):
        out.write("""static void
%(class_name)s_dealloc(py%(class_name)s *self) {
if(self->base) talloc_free(self->base);
};\n
""" % dict(class_name = self.class_name))

class ClassGenerator:
    def __init__(self, class_name, base_class_name, module):
        self.class_name = class_name
        self.methods = []
        self.module = module
        self.constructor = None
        self.base_class_name = base_class_name
        self.attributes = GetattrMethod(self.class_name, self.base_class_name)
        self.modifier = ''

    def is_active(self):
        """ Returns true if this class is active and should be generated """
        if self.modifier and ('PRIVATE' in self.modifier \
                                  or 'ABSTRACT' in self.modifier):
            log("%s is not active %s" % (self.class_name, self.modifier))
            return False

        return True

    def clone(self, new_class_name):
        """ Creates a clone of this class - usefull when implementing
        class extensions
        """
        result = ClassGenerator(new_class_name, self.class_name, self.module)
        result.constructor = self.constructor.clone(new_class_name)
        result.methods = [ x.clone(new_class_name) for x in self.methods ]
        result.attributes = self.attributes.clone(new_class_name)

        return result

    def add_method(self, method_name, args, return_type, docstring):
        result = Method(self.class_name, self.base_class_name,
                                   method_name, args, return_type,
                                   myclass = self)

        result.docstring = docstring
        self.methods.append(result)

    def add_attribute(self, attr_name, attr_type):
        try:
            ## All attribute references are always borrowed - that
            ## means we dont want to free them after accessing them
            type_class = dispatch(attr_name, "BORROWED "+attr_type)
        except KeyError:
            log("Unknown attribute type %s for  %s.%s" % (attr_type,
                                                          self.class_name,
                                                          attr_name))
            return

        self.attributes.add_attribute(type_class)

    def add_constructor(self, method_name, args, return_type, docstring):
        self.constructor = ConstructorMethod(self.class_name, self.base_class_name,
                                             method_name, args, return_type,
                                             myclass = self)
        self.constructor.docstring = docstring

    def struct(self,out):
        out.write("""\ntypedef struct {
  PyObject_HEAD
  %(class_name)s base;
} py%(class_name)s; \n
""" % dict(class_name=self.class_name))

    def code(self, out):
        if not self.constructor:
            raise RuntimeError("No constructor found for class %s" % self.class_name)

        self.constructor.write_destructor(out)
        self.constructor.write_definition(out)
        self.attributes.write_definition(out)

        for m in self.methods:
            m.write_definition(out)

    def initialise(self):
        return "python_wrappers[TOTAL_CLASSES].class_ref = (Object)&__%s;\n" \
            "python_wrappers[TOTAL_CLASSES++].python_type = &%s_Type;\n" % (
            self.class_name, self.class_name)

    def PyMethodDef(self, out):
        out.write("static PyMethodDef %s_methods[] = {\n" % self.class_name)
        for method in self.methods:
            method_name = method.name
            docstring = method.comment() + "\n\n" + method.docstring
            out.write('     {"%s",(PyCFunction)py%s_%s, METH_VARARGS|METH_KEYWORDS, "%s"},\n' % (
                    method_name,
                    self.class_name,
                    method_name, escape_for_string(docstring)))
        out.write("     {NULL}  /* Sentinel */\n};\n")

    def prototypes(self, out):
        """ Write prototype suitable for .h file """
        out.write("""staticforward PyTypeObject %s_Type;\n""" % self.class_name)
        self.constructor.prototype(out)
        self.attributes.prototype(out)
        for method in self.methods:
            method.prototype(out)

    def PyTypeObject(self, out):
        out.write("""
static PyTypeObject %(class)s_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                         /* ob_size */
    "%(module)s.%(class)s",               /* tp_name */
    sizeof(py%(class)s),            /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)%(class)s_dealloc,/* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_compare */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash */
    0,                         /* tp_call */
    0,                         /* tp_str */
    (getattrofunc)%(class)s_getattr,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,        /* tp_flags */
    "%(docstring)s",     /* tp_doc */
    0,	                       /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    %(class)s_methods,            /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)py%(class)s_init,      /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};
""" % {'class':self.class_name, 'module': self.module.name, 
       'docstring': "%s: %s" % (self.class_name, 
                                escape_for_string(self.docstring))})

class StructGenerator(ClassGenerator):
    """ A wrapper generator for structs """
    def __init__(self, class_name, base_class_name, module):
        self.class_name = class_name
        self.methods = []
        self.module = module
        self.constructor = StructConstructor(class_name, base_class_name,
                                             'Con', [], "void", myclass = self)
        self.base_class_name = base_class_name
        self.attributes = GetattrMethod(self.class_name, self.base_class_name)

    def struct(self, out):
        out.write("""\ntypedef struct {
  PyObject_HEAD
  %(class_name)s *base;
} py%(class_name)s; \n
""" % dict(class_name=self.class_name))

    def initialise(self):
        return ''

class parser:
    class_re = re.compile(r"^([A-Z]+)?\s*CLASS\(([A-Z_a-z0-9]+)\s*,\s*([A-Z_a-z0-9]+)\)")
    method_re = re.compile(r"^\s*([0-9A-Z_a-z ]+\s+\*?)METHOD\(([A-Z_a-z0-9]+),\s*([A-Z_a-z0-9]+),?")
    arg_re = re.compile(r"\s*([0-9A-Z a-z_]+\s+\*?)([0-9A-Za-z_]+),?")
    constant_re = re.compile(r"#define\s+([A-Z_0-9]+)\s+[^\s]+")
    struct_re = re.compile(r"([A-Z]+)\s+typedef struct\s+([A-Z_a-z0-9]+)\s+{")
    end_class_re = re.compile("END_CLASS")
    attribute_re = re.compile(r"^\s*([0-9A-Z_a-z ]+\s+\*?)\s*([A-Z_a-z]+)\s*;")
    comment_re = re.compile(r"^\s*//")
    comment_start_re = re.compile(r"/\*+")
    comment_end_re = re.compile(r"\*+/")
    blank_line_re = re.compile("\s+")
    current_class = None

    def __init__(self, module, verbosity=0):
        self.module = module
        self.current_comment = ''
        self.verbosity = verbosity
        global DEBUG

        DEBUG = verbosity

    def add_class(self, class_name, base_class_name, class_type, handler, docstring, modifier):
        try:
            self.current_class = self.module.classes[base_class_name].clone(class_name)
        except KeyError:
            log("Base class %s is not defined !!!!" % base_class_name)
            self.current_class = class_type(class_name, base_class_name, self.module)

        ## Now add the new class to the module object
        self.current_class.docstring = docstring
        self.current_class.modifier = modifier
        self.module.add_class(self.current_class, handler)

    def parse(self, filename):
        self.module.headers += '#include "%s"\n' % filename
        fd = open(filename)

        while 1:
            line = fd.readline()
            if not line: break

            ## Handle c++ style comments //
            m = self.comment_re.match(line)
            if m:

                self.current_comment = line[m.end():]
                while 1:
                    line = fd.readline()

                    m = self.comment_re.match(line)
                    if not m:
                        break

                    self.current_comment += line[m.end():]

            ## Multiline C style comments
            m = self.comment_start_re.search(line)
            if m:
                line = line[m.end():]
                while 1:
                    m = self.comment_end_re.search(line)
                    if m:
                        self.current_comment += line[:m.start()]
                        line = fd.readline()
                        break
                    else:
                        self.current_comment += line

                    line = fd.readline()
                    if not line: break

            m = self.constant_re.search(line)
            if m:
                ## We need to determine if it is a string or integer
                if re.search('"', line):
                    ## Its a string
                    self.module.add_constant(m.group(1), 'string')
                else:
                    self.module.add_constant(m.group(1), 'numeric')

            ## Wrap structs
            m = self.struct_re.search(line)
            if m:
                modifier = m.group(1)
                class_name = m.group(2)
                base_class_name = None
                ## Structs may be refered to as a pointer or absolute
                ## - its the same thing ultimatley.

                ## We only wrap structures which are explicitely bound
                if 'BOUND' in modifier:
                    self.add_class(class_name, base_class_name, StructGenerator, StructWrapper,
                                   self.current_comment, modifier)
                    type_dispatcher["%s *" % class_name] = PointerStructWrapper

                continue

            m = self.class_re.search(line)
            if m:
                ## We need to make a new class now... We basically
                ## need to build on top of previously declared base
                ## classes - so we try to find base classes, clone
                ## them if possible:
                modifier = m.group(1)
                class_name = m.group(2)
                base_class_name = m.group(3)
                self.add_class(class_name, base_class_name, ClassGenerator, Wrapper,
                               self.current_comment, modifier)
                continue

            m = self.method_re.search(line)
            if self.current_class and m:
                args = []
                method_name = m.group(3)
                return_type = m.group(1).strip()
                ## Now parse the args
                offset = m.end()
                while 1:
                    m = self.arg_re.match(line[offset:])
                    if not m:
                        ## Allow multiline definitions if there is \\
                        ## at the end of the line
                        if line.strip().endswith("\\"):
                            line = fd.readline()
                            offset = 0
                            if line:
                                continue

                        break

                    offset += m.end()
                    args.append([m.group(1).strip(), m.group(2).strip()])

                if return_type == self.current_class.class_name and \
                        not self.current_class.constructor:
                    self.current_class.add_constructor(method_name, args, return_type,
                                                       self.current_comment)
                else:
                    self.current_class.add_method(method_name, args, return_type,
                                                  self.current_comment)

            m = self.attribute_re.search(line)
            if self.current_class and m:
                type = m.group(1)
                name = m.group(2)
                self.current_class.add_attribute(name, type)

            m = self.end_class_re.search(line)
            if m:
                ## Just clear the current class context
                self.current_class = None

            ## We only care about comment immediately above methods
            ## etc as we take them to be documentation. If we get here
            ## the comment is not above anything we care about - so we
            ## clear it:
            self.current_comment = ''

    def write(self, out):
        self.module.write(out)

if __name__ == '__main__':
    p = parser(Module("pyaff4"))
    for arg in sys.argv[1:]:
        p.parse(arg)

    p.write(sys.stdout)
