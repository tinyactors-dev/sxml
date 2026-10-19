<?xml version="1.0"?>
<!-- One result document per book, and an index as the principal result
     (sxml transform with a result directory, apps/sxml/README.md). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template match="/">
    <xsl:for-each select="//book">
      <xsl:result-document href="books/{@id}.xml" method="xml" indent="yes">
        <book title="{title}"/>
      </xsl:result-document>
      <xsl:value-of select="'books/' || @id || '.xml&#10;'"/>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>
