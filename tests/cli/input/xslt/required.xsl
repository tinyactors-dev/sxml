<?xml version="1.0"?>
<!-- A required stylesheet parameter: XTDE0050 without a value. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:param name="count" as="xs:integer" required="yes"/>
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:value-of select="$count * 2"/>
  </xsl:template>
</xsl:stylesheet>
