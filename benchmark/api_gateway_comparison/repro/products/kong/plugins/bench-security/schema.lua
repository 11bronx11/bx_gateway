local typedefs = require "kong.db.schema.typedefs"

return {
  name = "bench-security",
  fields = {
    { consumer = typedefs.no_consumer },
    { protocols = typedefs.protocols_http },
    {
      config = {
        type = "record",
        fields = {
          { required_scope = { type = "string", default = "read", required = true } },
        },
      },
    },
  },
}
