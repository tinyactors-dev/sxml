// The EXI oracle (README.md): encodes and decodes with EXIficient, so that
// sxml's EXI codec is checked against an independent implementation.
//
//   java Oracle batch MANIFEST OUTPUT_DIRECTORY
//
// MANIFEST has one case per line, tab-separated: an identifier, an input
// file and an option specification (tests/exi/README.md, "Options"). For
// each case the oracle writes OUTPUT_DIRECTORY/<identifier>.exi (the input,
// encoded) and OUTPUT_DIRECTORY/<identifier>.xml (that stream, decoded
// again), and one line to OUTPUT_DIRECTORY/results.txt: the identifier and
// "ok", or "refused" and EXIficient's message. An input ending in .exi is
// decoded only.
import com.siemens.ct.exi.core.CodingMode;
import com.siemens.ct.exi.core.EXIFactory;
import com.siemens.ct.exi.core.EncodingOptions;
import com.siemens.ct.exi.core.FidelityOptions;
import com.siemens.ct.exi.core.grammars.Grammars;
import com.siemens.ct.exi.core.helpers.DefaultEXIFactory;
import com.siemens.ct.exi.grammars.GrammarFactory;
import com.siemens.ct.exi.main.api.sax.EXIResult;
import com.siemens.ct.exi.main.api.sax.EXISource;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import javax.xml.namespace.QName;
import javax.xml.transform.OutputKeys;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.sax.SAXSource;
import javax.xml.transform.stream.StreamResult;
import org.xml.sax.InputSource;
import org.xml.sax.XMLReader;
import org.xml.sax.helpers.XMLReaderFactory;

public final class Oracle {
    private static final Map<String, Grammars> schemas = new HashMap<>();

    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 3 || !arguments[0].equals("batch")) {
            System.err.println("usage: java Oracle batch MANIFEST OUTPUT_DIRECTORY");
            System.exit(2);
        }
        Path output = Paths.get(arguments[2]);
        Files.createDirectories(output);
        List<String> lines = Files.readAllLines(Paths.get(arguments[1]), StandardCharsets.UTF_8);
        try (PrintWriter results = new PrintWriter(Files.newBufferedWriter(output.resolve("results.txt")))) {
            for (String line : lines) {
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                String[] fields = line.split("\t", -1);
                String identifier = fields[0];
                try {
                    byte[] input = Files.readAllBytes(Paths.get(fields[1]));
                    byte[] stream = fields[1].endsWith(".exi") ? input : encode(factory(fields[2]), input);
                    byte[] decoded = decode(factory(fields[2]), stream);
                    Path exiFile = output.resolve(identifier + ".exi");
                    Files.createDirectories(exiFile.getParent());
                    Files.write(exiFile, stream);
                    Files.write(output.resolve(identifier + ".xml"), decoded);
                    results.println(identifier + "\tok");
                } catch (Exception | StackOverflowError failure) {
                    String message = String.valueOf(failure.getMessage()).replaceAll("\\s+", " ");
                    results.println(identifier + "\trefused\t" + failure.getClass().getSimpleName() + ": " + message);
                }
            }
        }
    }

    // The option specification: key[=value] pairs separated by ";".
    static EXIFactory factory(String specification) throws Exception {
        EXIFactory factory = DefaultEXIFactory.newInstance();
        FidelityOptions fidelity = FidelityOptions.createDefault();
        EncodingOptions encoding = EncodingOptions.createDefault();
        // An xsi:schemaLocation attribute is data like any other: keep it,
        // as sxml's encoder does (EXIficient drops it by default).
        encoding.setOption(EncodingOptions.INCLUDE_XSI_SCHEMALOCATION);
        for (String part : specification.split(";")) {
            if (part.isEmpty()) {
                continue;
            }
            String[] pair = part.split("=", 2);
            String key = pair[0];
            String value = pair.length > 1 ? pair[1] : "";
            switch (key) {
                case "alignment" -> factory.setCodingMode(switch (value) {
                    case "bit-packed" -> CodingMode.BIT_PACKED;
                    case "byte-alignment" -> CodingMode.BYTE_PACKED;
                    case "pre-compression" -> CodingMode.PRE_COMPRESSION;
                    case "compression" -> CodingMode.COMPRESSION;
                    default -> throw new IllegalArgumentException("alignment " + value);
                });
                case "preserve" -> {
                    for (String feature : value.split(",")) {
                        fidelity.setFidelity(switch (feature) {
                            case "comments" -> FidelityOptions.FEATURE_COMMENT;
                            case "pis" -> FidelityOptions.FEATURE_PI;
                            case "dtd" -> FidelityOptions.FEATURE_DTD;
                            case "prefixes" -> FidelityOptions.FEATURE_PREFIX;
                            case "lexical-values" -> FidelityOptions.FEATURE_LEXICAL_VALUE;
                            default -> throw new IllegalArgumentException("preserve " + feature);
                        }, true);
                    }
                }
                case "strict" -> fidelity.setFidelity(FidelityOptions.FEATURE_STRICT, true);
                case "fragment" -> factory.setFragment(true);
                case "self-contained" -> {
                    fidelity.setFidelity(FidelityOptions.FEATURE_SC, true);
                    List<QName> names = new ArrayList<>();
                    for (String name : value.split(",")) {
                        names.add(QName.valueOf(name));
                    }
                    factory.setSelfContainedElements(names.toArray(new QName[0]));
                }
                case "block-size" -> factory.setBlockSize(Integer.parseInt(value));
                case "value-max-length" -> factory.setValueMaxLength(Integer.parseInt(value));
                case "value-partition-capacity" -> factory.setValuePartitionCapacity(Integer.parseInt(value));
                case "schema" -> factory.setGrammars(schemas.computeIfAbsent(value, Oracle::grammars));
                case "xsd-types-only" -> factory.setGrammars(GrammarFactory.newInstance().createXSDTypesOnlyGrammars());
                case "header" -> {
                    for (String item : value.split(",")) {
                        encoding.setOption(switch (item) {
                            case "cookie" -> EncodingOptions.INCLUDE_COOKIE;
                            case "options" -> EncodingOptions.INCLUDE_OPTIONS;
                            case "schema-id" -> EncodingOptions.INCLUDE_SCHEMA_ID;
                            default -> throw new IllegalArgumentException("header " + item);
                        });
                    }
                }
                case "canonical" -> encoding.setOption(EncodingOptions.CANONICAL_EXI);
                case "utc-time" -> encoding.setOption(EncodingOptions.UTC_TIME);
                case "insignificant-xsi-nil" -> encoding.setOption(EncodingOptions.INCLUDE_INSIGNIFICANT_XSI_NIL);
                default -> throw new IllegalArgumentException("unknown option " + key);
            }
        }
        factory.setFidelityOptions(fidelity);
        factory.setEncodingOptions(encoding);
        return factory;
    }

    static Grammars grammars(String path) {
        try {
            return GrammarFactory.newInstance().createGrammars(path);
        } catch (Exception failure) {
            throw new IllegalStateException(failure);
        }
    }

    static byte[] encode(EXIFactory factory, byte[] xml) throws Exception {
        ByteArrayOutputStream stream = new ByteArrayOutputStream();
        EXIResult result = new EXIResult(factory);
        result.setOutputStream(stream);
        XMLReader reader = XMLReaderFactory.createXMLReader();
        reader.setFeature("http://xml.org/sax/features/namespaces", true);
        reader.setFeature("http://xml.org/sax/features/namespace-prefixes", false);
        reader.setContentHandler(result.getHandler());
        reader.setProperty("http://xml.org/sax/properties/lexical-handler", result.getHandler());
        reader.parse(new InputSource(new ByteArrayInputStream(xml)));
        return stream.toByteArray();
    }

    static byte[] decode(EXIFactory factory, byte[] exi) throws Exception {
        ByteArrayOutputStream xml = new ByteArrayOutputStream();
        Transformer transformer = TransformerFactory.newInstance().newTransformer();
        transformer.setOutputProperty(OutputKeys.ENCODING, "UTF-8");
        transformer.setOutputProperty(OutputKeys.OMIT_XML_DECLARATION, factory.isFragment() ? "yes" : "no");
        InputStream input = new ByteArrayInputStream(exi);
        EXISource source = new EXISource(factory);
        SAXSource sax = new SAXSource(source.getXMLReader(), new InputSource(input));
        transformer.transform(sax, new StreamResult(xml));
        return xml.toByteArray();
    }
}
