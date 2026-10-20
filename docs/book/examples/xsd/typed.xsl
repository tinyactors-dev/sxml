<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:import-schema schema-location="library.xsd"/>
  <xsl:output method="text"/>

  <!-- Validate the input, so its values have the schema's types. -->
  <xsl:template match="/">
    <xsl:variable name="library" as="document-node(element(library))">
      <xsl:document validation="strict">
        <xsl:copy-of select="."/>
      </xsl:document>
    </xsl:variable>
    <xsl:apply-templates select="$library//book"/>
  </xsl:template>

  <!-- Matches books whose price was validated as the type price. -->
  <xsl:template match="book[price instance of element(*, price)]">
    <xsl:text>{title}: {price + 1}&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
