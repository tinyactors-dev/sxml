<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="xml" indent="yes" omit-xml-declaration="yes"/>

  <xsl:template match="/article">
    <article>
      <xsl:for-each-group select="*" group-starting-with="h1">
        <section title="{current-group()[1]}">
          <xsl:copy-of select="current-group()[position() gt 1]"/>
        </section>
      </xsl:for-each-group>
    </article>
  </xsl:template>
</xsl:stylesheet>
