# inja plugin for protoc

The protoc program is used to turn [protobuf](https://developers.google.com/protocol-buffers) file containing Message and RPC-Call definitions into sourcecode of a given target language.
It is possible to write custom [plugins](https://developers.google.com/protocol-buffers/docs/reference/cpp/google.protobuf.compiler.plugin.html) that use the definitions in the protobuf file to generate arbitrary code.
The inja plugin permits specifying a template using the [inja-syntax](https://github.com/pantor/inja) and the file to write to with the additional options: `--inja_opt=Template=...,Out=...`
See the section `RPCCODE GENERATION` in CMakeLists.txt for example invocations.
JSON of the following format will be passed to the template:
```json
Services: {
    "Service1":
    {
        "Method1":
        {
            "In": "InputType1",
            "Out": "OutputType1"
        },
        "Method2": {...},
        ...
    },
    "Service2": {...},
    ...
}
```
