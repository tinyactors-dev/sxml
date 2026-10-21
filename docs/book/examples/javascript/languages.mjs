import { readFileSync } from 'node:fs';
import { load } from '@tinyactors/sxml';

// The one module has XQuery, XSLT and XSD too.
const sxml = await load();
const processor = sxml.createProcessor();

// XQuery over OTLP JSON: JavaScript objects and arrays become maps and
// arrays, so the parsed file can be a variable.
const query = processor.compileQuery(`
  declare option output:method "json";
  declare variable $request external;
  array {
    for member $resource in $request?resourceSpans
    let $service := $resource?resource?attributes?*[?key = "service.name"]?value?stringValue
    return { "service": $service, "spans": count($resource?scopeSpans?*?spans?*) }
  }`);
const request = JSON.parse(readFileSync('before.json', 'utf8'));
console.log(query.run({ variables: { request } }));

// XSLT: compile a stylesheet once, transform many documents.
const draft = processor.parse(readFileSync('vane.xml', 'utf8'));
const stylesheet = processor.compileStylesheet(`
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template match="/"><xsl:apply-templates select="//section[t/bcp14 = 'MUST']"/></xsl:template>
  <xsl:template match="section">{name}: {count(t[bcp14 = 'MUST'])} MUST&#10;</xsl:template>
</xsl:stylesheet>`);
process.stdout.write(stylesheet.transform({ source: draft }).output);

// XSD: an invalid document is a result, not an exception.
const schema = processor.compileSchema(`
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="bcp14">
    <xs:simpleType>
      <xs:restriction base="xs:token">
        <xs:enumeration value="MUST"/><xs:enumeration value="SHOULD"/><xs:enumeration value="MAY"/>
      </xs:restriction>
    </xs:simpleType>
  </xs:element>
</xs:schema>`);
const report = schema.validate('<bcp14>MIGHT</bcp14>');
console.log(report.validity, report.reports.map((each) => each.code));

processor.dispose();
