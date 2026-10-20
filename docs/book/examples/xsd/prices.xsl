<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    exclude-result-prefixes="xs">
  <xsl:import-schema schema-location="library.xsd"/>
  <xsl:output method="xml" indent="yes"/>

  <!-- A typed parameter: a rate, as a decimal. -->
  <xsl:param name="rate" as="xs:decimal" select="1.10"/>

  <!-- Validate the input with xsl:copy-of, into a typed variable. -->
  <xsl:template match="/">
    <xsl:variable name="typed" as="document-node(schema-element(library))">
      <xsl:copy-of select="." validation="strict"/>
    </xsl:variable>
    <xsl:apply-templates select="$typed/library"/>
  </xsl:template>

  <xsl:template match="schema-element(library)">
    <prices>
      <xsl:apply-templates select="shelf/book"/>
    </prices>
  </xsl:template>

  <!-- Each new price is made with the schema's type price, so it is
       validated as it is made. -->
  <xsl:template match="element(*, book)">
    <xsl:element name="price" type="price">
      <xsl:attribute name="currency" select="price/@currency"/>
      <xsl:sequence select="round(price * $rate, 2)"/>
    </xsl:element>
  </xsl:template>
</xsl:stylesheet>
