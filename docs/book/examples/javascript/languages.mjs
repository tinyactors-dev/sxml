import { load } from '@tinyactors/sxml';

// The one module has XQuery, XSLT and XSD too.
const sxml = await load();
const processor = sxml.createProcessor();
const library = processor.parse(`<books>
  <book year="1979"><title>Godel, Escher, Bach</title></book>
  <book year="1965"><title>Dune</title></book>
</books>`);

// XQuery: the prolog's output declarations decide the serialization.
const query = processor.compileQuery(`
  declare option output:method "json";
  declare variable $since external := 0;
  array { //book[@year >= $since] ! string(title) }`);
console.log(query.run({ context: library, variables: { since: 1970 } }));

// XSLT: compile a stylesheet once, transform many documents.
const stylesheet = processor.compileStylesheet(`
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template match="/"><xsl:apply-templates select="//book"/></xsl:template>
  <xsl:template match="book">{title} ({@year})&#10;</xsl:template>
</xsl:stylesheet>`);
process.stdout.write(stylesheet.transform({ source: library }).output);

// XSD: an invalid document is a result, not an exception.
const schema = processor.compileSchema(`
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="book">
    <xs:complexType>
      <xs:sequence><xs:element name="title" type="xs:string"/></xs:sequence>
      <xs:attribute name="year" type="xs:gYear" use="required"/>
    </xs:complexType>
  </xs:element>
</xs:schema>`);
const report = schema.validate('<book year="soon"><title>Dune</title></book>');
console.log(report.validity, report.reports.map((each) => each.code));

processor.dispose();
