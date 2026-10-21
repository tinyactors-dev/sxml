<?xml version="1.0"?>
<!-- A stylesheet parameter of a schema type (xsl:import-schema): a
     value the param option gives is cast to it, facets included (XSLT
     4.0, 9.5). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:import-schema schema-location="../xsd/types/book.xsd"/>
  <xsl:param name="price" as="price" required="yes"/>
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:value-of select="$price instance of price, $price * 2"/>
  </xsl:template>
</xsl:stylesheet>
