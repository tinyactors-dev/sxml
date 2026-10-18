<?xml version="1.0"?>
<!-- Reads a document above its own directory: relative references resolve
     against the stylesheet's path (docs/CLI.md, sxml transform). -->
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:value-of select="count(doc('../books.xml')//book)"/>
  </xsl:template>
</xsl:stylesheet>
