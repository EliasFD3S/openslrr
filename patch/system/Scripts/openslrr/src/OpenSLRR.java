package java.openslrr;

// OpenSLRR mod API (#487). A mod checks apiVersion() before it uses a class
// of this package; the version grows when the API changes (include/slrr/
// mod_api.hpp, kModApiVersion). Reference: docs/scripting/mod-api.md.
public class OpenSLRR
{
	public static native int apiVersion();
}
